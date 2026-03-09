// Collision + determinism test for lazy_hash over the full station-name universe
// (weather_stations.csv, superset of what gen.py samples).
//
// Determinism: lazy_hash may read only the name's own bytes. A hash that reads
// past the name picks up neighboring line bytes, so the same station hashes
// differently on different rows — that is a bug, and re-hashing the same name
// embedded in two different contexts catches it.
//
// Usage: hash_test [names_file]         exit 0 = no collisions, deterministic
// Default: tests/stations_10k.txt = the exact station list gen.py samples (seed 42).
// Also accepts weather_stations.csv for the full 41k-name universe.
// Regenerate the list:  awk -F';' '!s[$1]++{print $1}' input/measurements_1e9.txt
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/helpers.cpp"

int main(int argc, char* argv[]) {
  const std::string path = argc > 1 ? argv[1] : "tests/stations_10k.txt";
  std::ifstream f(path);
  if (!f) { std::cerr << "cannot open " << path << '\n'; return 2; }

  std::vector<std::string> names;
  {
    std::unordered_map<std::string, bool> seen;
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::string name = line.substr(0, line.find(';'));
      if (!seen.emplace(name, true).second) continue;
      names.push_back(name);
    }
  }

  int fail = 0;

  // determinism: same name, two different surroundings (mimics rows mid-file)
  for (const auto& n : names) {
    std::string a = "AAAAAAAA" + n + ";12.3\nBBBBBBBB";
    std::string b = "CCCCCCCC" + n + ";-4.5\nDDDDDDDD";
    uint64_t ha = lazy_hash({a.data() + 8, n.size()});
    uint64_t hb = lazy_hash({b.data() + 8, n.size()});
    if (ha != hb) {
      if (fail < 5)
        std::cerr << "non-deterministic: \"" << n << "\" (len " << n.size()
                  << ") " << std::hex << ha << " != " << hb << std::dec << '\n';
      fail++;
    }
  }
  if (fail) std::cerr << fail << " non-deterministic name(s)\n";

  // collisions: every distinct name must get a distinct key
  int coll = 0;
  std::unordered_map<uint64_t, const std::string*> by_key;
  for (const auto& n : names) {
    std::string buf = "AAAAAAAA" + n + ";12.3\nBBBBBBBB";
    uint64_t h = lazy_hash({buf.data() + 8, n.size()});
    auto [it, fresh] = by_key.emplace(h, &n);
    if (!fresh) {
      if (coll < 10)
        std::cerr << "collision: \"" << *it->second << "\" vs \"" << n << "\" -> "
                  << std::hex << h << std::dec << '\n';
      coll++;
    }
  }

  std::cout << names.size() << " names, " << coll << " collision(s), "
            << fail << " non-deterministic\n";
  return (coll || fail) ? 1 : 0;
}
