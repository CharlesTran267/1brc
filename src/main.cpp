#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <thread>
#include <unordered_map>
#include <absl/container/flat_hash_map.h>
#include <absl/container/btree_map.h>

#include "helpers.cpp"
#include "file_reader.cpp"

#ifndef DEBUG
#define DEBUG
#endif

// dataset size: 10^SIZE_EXP rows. Must match what gen.py produced.
#define SIZE_EXP 8
#define STR_(x) #x
#define XSTR_(x) STR_(x)
#define SIZE_TAG "1e" XSTR_(SIZE_EXP)
#define USE_STL 0

#ifndef NUM_THREADS
#define NUM_THREADS 8
#endif

constexpr auto INPUT_FILE = "input/measurements_" SIZE_TAG ".txt";
constexpr auto OUTPUT_FILE = "output/processed_output_" SIZE_TAG ".txt";
constexpr auto GOLDEN_OUTPUT = "output/golden_output_" SIZE_TAG ".txt";
constexpr size_t pow10(int e) { return e ? 10 * pow10(e - 1) : 1; }
constexpr size_t INPUT_FILE_LENGTH = pow10(SIZE_EXP);

class Solver {
  struct Stat {
    int16_t min;
    int16_t max;
    int32_t sum;
    uint16_t count;
  };

  using K = std::string_view;
#if USE_STL
  using Map = std::map<K, Stat>;
  using UMap = std::unordered_map<K, Stat>;
#else
  using Map = absl::btree_map<K, Stat>;
  using UMap = absl::flat_hash_map<K, Stat>;
#endif
  using It = std::string_view::iterator;

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

 public:
  Solver(const std::string& file_name) : m_reader(file_name) {
    Timer timer("Total solve");

    if constexpr (NUM_THREADS == 1) {
      thread_task(0, {m_reader.content.begin(), m_reader.content.end()});
      return;
    }

    std::array<std::pair<It, It>, NUM_THREADS> byte_marks{};
    std::size_t chunk = m_reader.size / NUM_THREADS;
    byte_marks.front().first = m_reader.content.begin();
    byte_marks.back().second = m_reader.content.end();
    for (std::size_t i = 1; i < NUM_THREADS; i++) {
      It start_byte = byte_marks[i - 1].first + chunk + 1;
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

  void thread_task(std::size_t idx, std::pair<It, It> bm) {
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

      auto it = m_maps[idx].find(name);
      if (it != m_maps[idx].end()) {
        Stat& st = it->second;
        st.count++;
        st.sum += val;
        st.max = std::max(st.max, val);
        st.min = std::min(st.min, val);
      } else {
        m_maps[idx].emplace(name, Stat{val, val, val, 1});
      }
      end_it++;
      start_it = end_it;
    }
  }

  void write(const std::string& output_file) {
#ifdef DEBUG
    Timer timer("Write time");
#endif

    Map combined_map;
    for (auto& m : m_maps) {
      for (auto& [k, v] : m) {
        if (!combined_map.count(k)) {
          combined_map[k] = std::move(v);
          continue;
        }

        combined_map[k].count += v.count;
        combined_map[k].sum += v.sum;
        combined_map[k].min = std::min(combined_map[k].min, v.min);
        combined_map[k].max = std::max(combined_map[k].max, v.max);
      }
    }

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
  std::array<UMap, NUM_THREADS> m_maps;
  std::array<std::thread, NUM_THREADS> m_thread_pool;
};

void process_line(const std::string& line) {}

int main(int argc, char* argv[]) {
  // first argument is number of threads
  try {
    Solver solver(INPUT_FILE);
    solver.write(OUTPUT_FILE);
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
