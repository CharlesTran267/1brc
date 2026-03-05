#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <immintrin.h>

#include "helpers.cpp"
#include "file_reader.cpp"
#include "flat_hash_map.cpp"

#ifndef DEBUG
#define DEBUG 1
#endif

// dataset size: 10^SIZE_EXP rows. Must match what gen.py produced.
#define SIZE_EXP 9
#define STR_(x) #x
#define XSTR_(x) STR_(x)
#define SIZE_TAG "1e" XSTR_(SIZE_EXP)
#ifndef CONTAINER_TYPE
#define CONTAINER_TYPE 2  // 0 for stl, 1 for absl, 2 for own
#endif
#ifndef MAP_SZ
#define MAP_SZ (1 << 16)  // own-map slots; 10k stations -> 61% load.
#endif
#ifndef BATCH
#define BATCH 8  // rows in flight per iteration; tune with -DBATCH=
#endif

#ifndef NUM_THREADS
#define NUM_THREADS 8
#endif

constexpr auto INPUT_FILE = "input/measurements_" SIZE_TAG ".txt";
constexpr auto OUTPUT_FILE = "output/processed_output_" SIZE_TAG ".txt";
constexpr auto GOLDEN_OUTPUT = "output/golden_output_" SIZE_TAG ".txt";
constexpr size_t pow10(int e) { return e ? 10 * pow10(e - 1) : 1; }
constexpr size_t INPUT_FILE_LENGTH = pow10(SIZE_EXP);

constexpr auto MAX_STATION = 10000;

#if CONTAINER_TYPE == 0
#include <map>
#include <unordered_map>

template <class K, class V>
using Map = std::map<K, V>;
template <class K, class V>
using UMap = std::unordered_map<K, V>;
#elif CONTAINER_TYPE == 1
#include <absl/container/flat_hash_map.h>
#include <absl/container/btree_map.h>

template <class K, class V>
using Map = absl::btree_map<K, V>;
template <class K, class V>
using UMap = absl::flat_hash_map<K, V>;
#else
#include <absl/container/btree_map.h>

template <class K, class V>
using Map = absl::btree_map<K, V>;
template <class K, class V, std::size_t SZ, auto fn>
using UMap = FlatHashMap<K, V, SZ, fn>;
#endif

using sit = std::string_view::iterator;
using psv = std::pair<sit, sit>;

struct Timer {
  Timer(const std::string& prefix = "")
      : m_pre(prefix), start(std::chrono::high_resolution_clock::now()) {}
  ~Timer() {
    // count in milliseconds
    auto end = std::chrono::high_resolution_clock::now();
    double duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();
    std::cout << m_pre << ':' << duration << " ms\n";
  }

 private:
  std::string m_pre;
  std::chrono::high_resolution_clock::time_point start;
};

// lazy_hash's low bits are near-raw name bytes; one multiply spreads them or
// linear probing clusters badly (measured: avg 115 probes/lookup vs 1.1).
uint64_t mix_hash(uint64_t h) { return (h * 0x9E3779B97F4A7C15ULL) >> 48; }

// One SSE compare covers names up to 16B (nearly all of them); the loop is
// the fallback for longer names. No call overhead, unlike memchr.
inline const char* find_semi(const char* p) {
  const __m128i semi = _mm_set1_epi8(';');
  for (;;) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    int m = _mm_movemask_epi8(_mm_cmpeq_epi8(v, semi));
    if (m) return p + __builtin_ctz(m);
    p += 16;
  }
}

// merykitty's SWAR parse: all four temp layouts (-XX.X, -X.X, XX.X, X.X)
// branchlessly from one 8-byte load. Returns tenths; sets nl to the '\n'.
// Digits have ASCII bit 4 set; '.', '-', '\n' do not — ctz on ~w finds the dot,
// whose position selects the shift that aligns any layout to one template.
inline int16_t parse_temp(const char* p, const char*& nl) {
  uint64_t w;
  std::memcpy(&w, p, 8);
  int64_t sgn = (~(int64_t)w << 59) >> 63;        // -1 if leading '-', else 0
  uint64_t nosign = w & ~(uint64_t)(sgn & 0xFF);  // blank the '-' byte
  int dot = __builtin_ctzll(~w & 0x10101000ULL);
  uint64_t digits = (nosign << (28 - dot)) & 0x0F000F0F00ULL;
  uint64_t abs_v = ((digits * 0x640A0001ULL) >> 32) & 0x3FF;
  nl = p + (dot >> 3) + 2;
  return (int16_t)((abs_v ^ sgn) - sgn);
}

class Solver {
  struct Stat {  // 16B: key+Stat = 32B slot, two per cache line, never split.
    int16_t min;
    int16_t max;
    uint32_t count;
    int64_t sum;
  };

  using K = uint64_t;
  using SMap = Map<K, Stat>;
#if CONTAINER_TYPE == 2
  using SUMap = UMap<K, Stat, MAP_SZ, mix_hash>;
#else
  using SUMap = UMap<K, Stat>;
#endif

 public:
  Solver(const std::string& file_name) : m_reader(file_name) {
#ifdef DEBUG
    Timer timer("Total process");
#endif

    if constexpr (NUM_THREADS == 1) {
      process_range(0, m_reader.content.begin(), m_reader.content.end());
      return;
    }

    for (std::size_t i = 0; i < NUM_THREADS; ++i) {
      auto t = std::thread(&Solver::thread_task, this, i);
      m_thread_pool[i] = std::move(t);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
      m_thread_pool[i].join();
    }
  }

  // Threads pull fixed-size chunks off an atomic cursor: fast cores take more
  // chunks, so nobody waits on the slowest core at the end (P/E hybrid).
  static constexpr std::size_t CHUNK_BYTES = 2 << 20;

  void thread_task(std::size_t idx) {
#ifdef DEBUG
    Timer timer("Time to process " + std::to_string(idx));
#endif
    const char* base = m_reader.content.data();
    const std::size_t size = m_reader.size;
    for (;;) {
      std::size_t begin =
          m_cursor.fetch_add(CHUNK_BYTES, std::memory_order_relaxed);
      if (begin >= size) break;
      std::size_t end = std::min(begin + CHUNK_BYTES, size);
      // a thread owns the lines that START inside its chunk; the last one may
      // run past `end`. First owned line starts right after the first '\n' at
      // or after begin-1 (begin 0 owns from byte 0).
      const char* s = base;
      if (begin) {
        s = static_cast<const char*>(
            std::memchr(base + begin - 1, '\n', end - begin + 1));
        if (!s) continue;  // chunk entirely inside one line
        s += 1;
      }
      process_range(idx, s, base + end);
    }
  }

  struct Row {
    uint64_t h;
    std::string_view name;
    int16_t val;
  };

  void process_range(std::size_t idx, const char* start_it,
                     const char* range_end) {
    const char* file_end = m_reader.content.end();
    Row batch[BATCH];

    while (start_it < range_end) {
      // Sweep A: scan + parse + hash + prefetch. Pure L1/register work, so the
      // BATCH slot loads below overlap instead of serializing one DRAM miss
      // per row.
      int n = 0;
      for (; n < BATCH && start_it < range_end; ++n) {
        auto deli_it = find_semi(start_it);
        Row& r = batch[n];
        const char* nl;
        r.val = parse_temp(deli_it + 1, nl);  // finds the '\n' as a side effect
        r.name = std::string_view(start_it, deli_it - start_it);
        r.h = lazy_hash(r.name);
#if CONTAINER_TYPE == 2
        m_maps[idx].prefetch(r.h);
#endif
        start_it = nl + 1;
      }

      // Sweep B: updates land on cache lines already in flight.
      for (int j = 0; j < n; ++j) {
        const Row& r = batch[j];
#if CONTAINER_TYPE == 2
        // fresh slot inits {val, val, 0, 0}; the update makes {val,val,1,val}
        Stat& st = m_maps[idx].upsert(r.h, r.val, r.val, 0, 0);
#else
        auto it = m_maps[idx].find(r.h);
        if (it == m_maps[idx].end())
          it = m_maps[idx].emplace(r.h, Stat{r.val, r.val, 0, 0}).first;
        Stat& st = it->second;
#endif
        // fresh slot (count still 0): remember the name once, off the hot path
        if (st.count == 0) [[unlikely]]
          m_names[idx].emplace(r.h, r.name);
        st.count++;
        st.sum += r.val;
        st.max = std::max(st.max, r.val);
        st.min = std::min(st.min, r.val);
      }
    }
  }

  void write(const std::string& output_file) {
    Map<std::string_view, Stat> combined_map;
    {
#ifdef DEBUG
      Timer timer("Aggregate time");
#endif
      for (auto& m : m_maps) {
        for (auto&& [h, v] : m) {
          auto sv = m_names[&m - m_maps.data()].at(h);
          auto it = combined_map.find(sv);
          if (it == combined_map.end()) {
            combined_map[sv] = {v.min, v.max, v.count, v.sum};
            continue;
          }
          combined_map[sv].count += v.count;
          combined_map[sv].sum += v.sum;
          combined_map[sv].min = std::min(combined_map[sv].min, v.min);
          combined_map[sv].max = std::max(combined_map[sv].max, v.max);
        }
      }
    }

#ifdef DEBUG
    Timer timer("Write time");
#endif
    std::ofstream out(output_file);
    if (!out.is_open()) {
      throw std::runtime_error("Failed to open file");
    }

    out << std::fixed << std::setprecision(1) << '{';
    for (auto it = combined_map.begin(); it != combined_map.end(); it++) {
      const auto& e = *it;
      out << e.first << '=' << ((float)e.second.min) / 10 << '/'
          << ((float)e.second.sum) / (e.second.count * 10) << '/'
          << ((float)e.second.max) / 10;
      if (std::next(it) != combined_map.end())
        out << ", ";
      else
        out << "}\n";
    }
  }

 private:
  FileReader m_reader;
  std::array<SUMap, NUM_THREADS> m_maps;
  std::array<std::unordered_map<uint64_t, std::string_view>, NUM_THREADS>
      m_names;  // cold: written ~10k times, read only at merge
  std::array<std::thread, NUM_THREADS> m_thread_pool;
  std::atomic<std::size_t> m_cursor{0};
};

int main(int argc, char* argv[]) {
  // first argument is number of threads
  try {
    {
      Timer timer("Total time");
      Solver solver(INPUT_FILE);
      solver.write(OUTPUT_FILE);
    }
    if (!compare(OUTPUT_FILE, GOLDEN_OUTPUT)) {
      std::cout << "OUTPUT does NOT match GOLDEN_OUTPUT\n";
    } else {
      std::cout << "OUTPUT matches GOLDEN_OUTPUT\n";
    }
  } catch (const std::exception& err) {
    std::cout << "Failed to solve:" << err.what() << '\n';
  }

  return 0;
}
