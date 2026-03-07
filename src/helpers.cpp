#include <cstdint>
#include <cstring>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/types.h>
#include <string>
#include <string_view>
#include <vector>

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

bool compare(const std::string& output_file, const std::string& golden_output) {
  std::ifstream f(golden_output + HASH_SUF);
  if (f) {
    return sha256_file(output_file) ==
           std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
  }
  return sha256_file(output_file) == sha256_file(golden_output);
}

int16_t from_chars_op(std::string_view::iterator b,
                      std::string_view::iterator e) {
  int16_t ans{};
  bool neg = false;
  if (*b == '-') {
    neg = true;
    b++;
  }

  for (auto it = b; it != e; it++) {
    if (*it == '.') continue;
    ans = ans * 10 + *it - '0';
  }
  if (ans < 1000) ans *= 10;
  return neg ? -ans : ans;
}

uint64_t lazy_hash(std::string_view name) {
  uint64_t f = 0, e = 0;
  if (name.size() >= 8) {
    std::memcpy(&f, name.data(), 8);
    std::memcpy(&e, name.data() + name.size() - 8, 8);
    return f ^ (e << 1);
  }
  std::memcpy(&f, name.data(), name.size());  // short name: the bytes are the key
  return f;
}
