#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

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
      thread_task(0, {m_reader.content.begin(), m_reader.content.end()});
      return;
    }

    std::array<psv, NUM_THREADS> byte_marks{};
    std::size_t chunk = m_reader.size / NUM_THREADS;
    byte_marks.front().first = m_reader.content.begin();
    byte_marks.back().second = m_reader.content.end();
    for (std::size_t i = 1; i < NUM_THREADS; i++) {
      auto start_byte = byte_marks[i - 1].first + chunk + 1;
      while ((*start_byte) != '\n') start_byte++;
      byte_marks[i].first = start_byte + 1;
      byte_marks[i - 1].second = byte_marks[i].first - 1;
    }

    for (std::size_t i = 0; i < NUM_THREADS; ++i) {
      auto t = std::thread(&Solver::thread_task, this, i, byte_marks[i]);
      m_thread_pool[i] = std::move(t);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
      m_thread_pool[i].join();
    }
  }

  void thread_task(std::size_t idx, psv bm) {
#ifdef DEBUG
    Timer timer("Time to process " + std::to_string(idx));
#endif

    auto start_it = bm.first;
    auto end_it = start_it, deli_it = start_it;

    while (start_it < bm.second) {
      // memchr is AVX2 in glibc: scans the name (the long part) 32B at a time.
      deli_it =
          static_cast<sit>(std::memchr(start_it, ';', bm.second - start_it));
      // temp is 3-5 bytes, so '\n' sits at deli+4..deli+6: <=2 scalar steps.
      end_it = deli_it + 4;
      while (*end_it != '\n') end_it++;

      std::string_view name(start_it, deli_it - start_it);
      std::string_view val_str(deli_it + 1, end_it - deli_it - 1);
      int16_t val = from_chars_op(val_str.begin(), val_str.end());

      auto h = lazy_hash(name);
#if CONTAINER_TYPE == 2
      // fresh slot inits {val, val, 0, 0}; the shared update makes
      // {val,val,1,val}
      Stat& st = m_maps[idx].upsert(h, val, val, 0, 0);
#else
      auto it = m_maps[idx].find(h);
      if (it == m_maps[idx].end())
        it = m_maps[idx].emplace(h, Stat{val, val, 0, 0}).first;
      Stat& st = it->second;
#endif
      // fresh slot (count still 0): remember the name once, off the hot path
      if (st.count == 0) [[unlikely]]
        m_names[idx].emplace(h, name);
      st.count++;
      st.sum += val;
      st.max = std::max(st.max, val);
      st.min = std::min(st.min, val);
      start_it = end_it + 1;
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
      out << e.first << '=' << ((float)e.second.min) / 100 << '/'
          << ((float)e.second.sum) / (e.second.count * 100) << '/'
          << ((float)e.second.max) / 100;
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
