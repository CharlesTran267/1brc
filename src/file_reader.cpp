#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
    // SWAR parsing overreads <=7B past the final '\n'; those bytes exist only
    // in the zero-fill of the last partial page.
    if ((size & 4095) == 0)
      throw std::runtime_error("page-aligned input size; append 1 byte");

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
