#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

/* Simple flat hash map that use
 * linear probing and stack allocation */
template <class K, class V, std::size_t SZ, auto HashFn>
class FlatHashMap {
  using KV = std::pair<K, V>;
  static constexpr int MASK = SZ - 1;
  static_assert(!(SZ & MASK), "size must be a power of two");

 public:
  FlatHashMap() : m_buf(SZ + 1), m_tags(SZ + 1, 0) {}

  // 64KB tag filter, L2-resident: probe reads 1 byte and touches the slot
  // array only on a tag hit -- same idea as absl's control bytes, minus SIMD.
  // |1 keeps a real tag from colliding with 0 = empty.
  static uint8_t tag_of(K k) { return uint8_t(k >> 32) | 1; }

  class iterator {
   public:
    bool operator!=(const iterator& other) { return i != other.i; }
    std::pair<const K&, V&> operator*() {
      return {m->m_buf[i].first, m->m_buf[i].second};
    }
    iterator operator++() {
      i++;
      skip();
      return *this;
    }

   private:
    friend class FlatHashMap;
    iterator(FlatHashMap* m, std::size_t i) : m(m), i(i) { skip(); }

    FlatHashMap* m;
    std::size_t i;

    void skip() {
      while (i < SZ && m->m_tags[i] == 0) ++i;
    }
  };

  iterator find(K k) {
    k += !k;  // keep in sync with upsert's remap
    auto i = find_until_empty(k);
    if (m_tags[i] == 0) return end();

    return iterator(this, i);
  }

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, SZ); }

  // hot-path: one probe, no iterator, no re-compare. Returns existing value
  // or default-constructs V(args...) in the slot first.
  template <class... Args>
  V& upsert(K k, Args&&... args) {
    k += !k;
    auto i = find_until_empty(k);
    if (m_tags[i] == 0) {
      m_tags[i] = tag_of(k);
      m_buf[i] = {k, V(args...)};
    }
    return m_buf[i].second;
  }

  template <class... Args>
  void emplace(K k, Args... args) {
    auto i = find_until_empty(k);

    m_buf[i] = {k, V(args...)};
  }

 private:
  std::vector<KV> m_buf;
  std::vector<uint8_t> m_tags;

  std::size_t find_until_empty(const K& k) {
    std::size_t i = HashFn(k) & (SZ - 1);
    auto ori = i;
    const uint8_t t = tag_of(k);
    while (m_tags[i] != 0 && (m_tags[i] != t || m_buf[i].first != k)) {
      i = (i + 1) & MASK;
      if (ori == i) throw std::runtime_error("Buffer is full!");
    }
    return i;
  }
};
