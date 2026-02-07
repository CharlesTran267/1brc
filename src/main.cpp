#include <openssl/err.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define INPUT_FILE "output/measurements.txt"
#define OUTPUT_FILE "output/processed_output.txt"
#define GOLDEN_OUTPUT "output/golden_output.txt"
#define NUM_THREADS 4
#define NUM_LINES 1_000_000

std::array<unsigned char, 32> sha256_file(const std::string& path) {
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

  std::array<unsigned char, 32> digest{};
  unsigned int len = 0;
  if (EVP_DigestFinal_ex(ctx, digest.data(), &len) != 1 || len != 32) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }

  EVP_MD_CTX_free(ctx);
  return digest;
}

class Solver {
  struct Stat {
    double min;
    double max;
    double sum;
    int count;
  };

  struct Timer {
    Timer() : start(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
      // count in milliseconds
      auto end = std::chrono::high_resolution_clock::now();
      double duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();
      std::cout << "Time taken: " << duration << " ms\n";
    }

   private:
    std::chrono::high_resolution_clock::time_point start;
  };

 public:
  Solver(const std::string& file_name) {
    std::ifstream input_file(file_name);
    if (!input_file.is_open()) {
      throw std::runtime_error("Failed to open input file!");
    }

    Timer timer;

    std::string line;
    while (std::getline(input_file, line)) {
      auto it = line.find(';');
      if (it == std::string::npos) {
        continue;
      }

      std::string key = line.substr(0, it);
      double value = std::stod(line.substr(it + 1));
      if (map.count(key)) {
        map[key].min = std::min(value, map[key].min);
        map[key].max = std::max(value, map[key].max);
        map[key].sum += value;
        ++map[key].count;
      } else {
        map[key] = {value, value, value, 1};
      }
    }
  }

  void write(const std::string& output_file) {
    std::ofstream out(output_file);
    if (!out.is_open()) {
      throw std::runtime_error("Failed to open file");
    }

    Timer timer;

    out << std::fixed << std::setprecision(1) << '{';
    for (auto it = map.begin(); it != map.end(); it++) {
      const auto& e = *it;
      out << e.first << '=' << e.second.min << '/'
          << e.second.sum / e.second.count << '/' << e.second.max;
      if (std::next(it) != map.end())
        out << ", ";
      else
        out << "}\n";
    }
  }

  static bool compare(const std::string& output_file,
                      const std::string& golden_output) {
    return sha256_file(output_file) == sha256_file(golden_output);
  }

 private:
  std::map<std::string, Stat> map;
};

void process_line(const std::string& line) {}

int main(int argc, char* argv[]) {
  // first argument is number of threads
  Solver solver(INPUT_FILE);
  solver.write(OUTPUT_FILE);
  if (!solver.compare(OUTPUT_FILE, GOLDEN_OUTPUT)) {
    std::cerr << "OUTPUT mismatch";
  } else {
    std::cout << "SUCESS!";
  }

  return 0;
}
