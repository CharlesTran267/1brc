#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <absl/container/flat_hash_map.h>
#include <absl/container/btree_map.h>

#include "helpers.cpp"
#include "file_reader.cpp"

#ifndef DEBUG
#define DEBUG 1
#endif

// dataset size: 10^SIZE_EXP rows. Must match what gen.py produced.
#define SIZE_EXP 9
#define STR_(x) #x
#define XSTR_(x) STR_(x)
#define SIZE_TAG "1e" XSTR_(SIZE_EXP)
#define USE_STL 0

#ifndef NUM_THREADS
#define NUM_THREADS 1
#endif

constexpr auto INPUT_FILE = "input/measurements_" SIZE_TAG ".txt";
constexpr auto OUTPUT_FILE = "output/processed_output_" SIZE_TAG ".txt";
constexpr auto GOLDEN_OUTPUT = "output/golden_output_" SIZE_TAG ".txt";
constexpr size_t pow10(int e) { return e ? 10 * pow10(e - 1) : 1; }
constexpr size_t INPUT_FILE_LENGTH = pow10(SIZE_EXP);

#if USE_STL
template <class K, class V>
using Map = std::map<K, V>;
template <class K, class V>
using UMap = std::unordered_map<K, V>;
#else
template <class K, class V>
using Map = absl::btree_map<K, V>;
template <class K, class V>
using UMap = absl::flat_hash_map<K, V>;
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

class Solver {
  struct Stat {
    int16_t min;
    int16_t max;
    int32_t sum;
    uint16_t count;
    std::string_view sv;
  };

  using K = uint64_t;
  using SMap = Map<K, Stat>;
  using SUMap = UMap<K, Stat>;

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
      while (*end_it != '\n') {
        if (*end_it == ';') deli_it = end_it;
        end_it++;
      }

      std::string_view name(start_it, deli_it - start_it);
      std::string_view val_str(deli_it + 1, end_it - deli_it - 1);
      int16_t val = from_chars_op(val_str.begin(), val_str.end());

      auto h = lazy_hash(name);
      auto it = m_maps[idx].find(h);
      if (it != m_maps[idx].end()) {
        Stat& st = it->second;
        st.count++;
        st.sum += val;
        st.max = std::max(st.max, val);
        st.min = std::min(st.min, val);
      } else {
        m_maps[idx].emplace(h, Stat{val, val, val, 1, name});
      }
      end_it++;
      start_it = end_it;
    }
  }

  void write(const std::string& output_file) {
    Map<std::string_view, Stat> combined_map;
    {
#ifdef DEBUG
      Timer timer("Aggregated time");
#endif
      for (auto& m : m_maps) {
        for (auto& [h, v] : m) {
          auto it = combined_map.find(v.sv);
          if (it == combined_map.end()) {
            combined_map[v.sv] = {v.min, v.max, v.sum, v.count, v.sv};
            continue;
          }
          combined_map[v.sv].count += v.count;
          combined_map[v.sv].sum += v.sum;
          combined_map[v.sv].min = std::min(combined_map[v.sv].min, v.min);
          combined_map[v.sv].max = std::max(combined_map[v.sv].max, v.max);
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
  std::array<UMap<uint64_t, std::string_view>, NUM_THREADS> m_name_maps;
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
