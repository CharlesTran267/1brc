#include <exception>
#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

constexpr bool DEBUG = 1;

// dataset size: 10^SIZE_EXP rows. Must match what gen.py produced.
#define SIZE_EXP 8
#define STR_(x) #x
#define XSTR_(x) STR_(x)
#define SIZE_TAG "1e" XSTR_(SIZE_EXP)

constexpr auto INPUT_FILE = "input/measurements_" SIZE_TAG ".txt";
constexpr auto OUTPUT_FILE = "output/processed_output_" SIZE_TAG ".txt";
constexpr auto GOLDEN_OUTPUT = "output/golden_output_" SIZE_TAG ".txt";
constexpr int NUM_THREADS = 8;
constexpr size_t pow10(int e) { return e ? 10 * pow10(e - 1) : 1; }
constexpr size_t INPUT_FILE_LENGTH = pow10(SIZE_EXP);
constexpr size_t CHUNK_SIZE = INPUT_FILE_LENGTH / NUM_THREADS;

constexpr auto HASH_SUF = ".hash";
std::string sha256_file(const std::string& path, bool cache = false) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot open file: " + path);

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestInit_ex failed");
  }

  constexpr size_t BUF = 1 << 20;  // 1 MB
  std::vector<char> buf(BUF);

  while (f) {
    f.read(buf.data(), buf.size());
    std::streamsize n = f.gcount();
    if (n > 0) {
      if (EVP_DigestUpdate(ctx, buf.data(), (size_t)n) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestUpdate failed");
      }
    }
  }

  std::string digest;
  digest.resize(32);
  unsigned int len = 0;
  if (EVP_DigestFinal_ex(ctx, (unsigned char*)digest.data(), &len) != 1 ||
      len != 32) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }

  EVP_MD_CTX_free(ctx);
  if (cache) {
    std::ofstream of(path + HASH_SUF);
    of << digest;
  }
  return digest;
}

struct FileReader {
  int fd;
  std::size_t size;
  std::string_view content;

  FileReader(const std::string& file) {
    fd = ::open(file.data(), O_RDONLY);
    if (fd < 0) {
      throw std::runtime_error("Failed to open file");
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      throw std::runtime_error("Failed to get file status");
    }

    size = static_cast<std::size_t>(st.st_size);
    if (size == 0) throw std::runtime_error("File is empty");

    void* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) throw std::runtime_error("mmap failed!");

    content = std::string_view(static_cast<char*>(p), size);
  }

  ~FileReader() {
    if (fd >= 0) ::close(fd);
    if (content.data() != nullptr) {
      munmap(const_cast<char*>(content.data()), size);
    }
  }
};

class Solver {
  struct Stat {
    double min;
    double max;
    double sum;
    int count;
  };

  using Map = std::map<std::string_view, Stat>;
  using UMap = std::unordered_map<std::string_view, Stat>;

  struct Timer {
    Timer(const std::string& prefix = "")
        : m_pre(prefix), start(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
      if constexpr (!DEBUG) return;

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
      thread_task(0, 0);
      return;
    }

    std::array<std::size_t, NUM_THREADS> byte_marks{};
    byte_marks[0] = 0;
    std::size_t cnt = 0;
    int thread_idx = 1;
    for (std::size_t i = 0; i < m_reader.size; ++i) {
      if (m_reader.content[i] == '\n') {
        cnt++;
        if (cnt == CHUNK_SIZE) {
          byte_marks[thread_idx++] = i + 1;
          cnt = 0;
          if (thread_idx == NUM_THREADS) break;
        }
      }
    }

    for (std::size_t i = 0; i < NUM_THREADS; ++i) {
      auto t = std::thread(&Solver::thread_task, this, i, byte_marks[i]);
      m_thread_pool[i] = std::move(t);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
      m_thread_pool[i].join();
    }
  }

  void thread_task(std::size_t idx, std::size_t byte_mark) {
    Timer timer("Time to process " + std::to_string(idx));
    auto start_it = m_reader.content.data() + byte_mark;
    for (int i = 0; i < CHUNK_SIZE && start_it < m_reader.content.end(); ++i) {
      auto end_it = std::find(start_it, m_reader.content.end(), '\n');
      auto deli_it = std::find(start_it, end_it, ';');
      std::string_view name(start_it, deli_it - start_it);
      std::string_view val_str(deli_it + 1, end_it - deli_it - 1);
      double val;
      auto [ptr, erc] = std::from_chars(val_str.begin(), val_str.end(), val);

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
      start_it = end_it + 1;
    }
  }

  void write(const std::string& output_file) {
    Timer timer("Write time");

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
      out << e.first << '=' << e.second.min << '/'
          << e.second.sum / e.second.count << '/' << e.second.max;
      if (std::next(it) != combined_map.end())
        out << ", ";
      else
        out << "}\n";
    }
  }

  static bool compare(const std::string& output_file,
                      const std::string& golden_output) {
    std::ifstream f(golden_output + HASH_SUF);
    if (f) {
      return sha256_file(output_file) ==
             std::string(std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>());
    }
    return sha256_file(output_file) == sha256_file(golden_output);
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
    if (!solver.compare(OUTPUT_FILE, GOLDEN_OUTPUT)) {
      std::cout << "OUTPUT does NOT match GOLDEN_OUTPUT\n";
    } else {
      std::cout << "OUTPUT matches GOLDEN_OUTPUT\n";
    }
  } catch (const std::exception& err) {
    std::cout << "Failed to solve:" << err.what() << '\n';
  }

  return 0;
}
