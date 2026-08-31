# 1BRC

My take on the [One Billion Row Challenge](https://github.com/gunnarmorling/1brc) in C++:
aggregate min/mean/max per station over 1e9 `station;temperature` lines (16 GB).

**Best: 2.06 s** — ~485M rows/s on a 13th-gen i7-1355U laptop (2 P + 8 E cores, 12 threads).

## Techniques, in the order they landed

| technique | why it helps | measured |
|---|---|---|
| mmap the input | no read syscalls, no buffer copies | baseline |
| threads + per-thread maps | no locks, merge 10k entries once at the end | ~7x over 1 thread |
| fixed-point `int` temps | float parsing was the #2 hotspot | ~2x |
| 16-byte head+tail key | u64 key instead of string compares (collision-checked against the full station list, `tests/hash_test.cpp`) | with map below |
| custom open-addressing map | one probe + 1-byte tag filter instead of `unordered_map` pointer chasing; ~absl speed with a `prefetch()` hook absl can't give | ~2.5x over `std::unordered_map` |
| chunk-stealing threads | 2MB chunks off an atomic counter, fast cores take more — static splits left P-cores idle 25% | 1.06x, unlocks 12 threads |
| batch + software prefetch | parse 8 rows, prefetch their slots, then update: DRAM misses overlap instead of serializing | 1.15x |
| merykitty SWAR parse | one 8-byte load parses any temp layout branchlessly, newline position falls out free | 1.28x |
| inline SSE `;` scan | one `cmpeq` covers almost every name, no memchr call overhead | 1.05x |

Tried and reverted (no measured win): SoA slot layout, huge pages, clang, larger batches,
double-buffered prefetch. Perfect hashing is impossible at 10k keys with a seeded multiply
(birthday bound: ~763 expected collisions per seed).

## Run it

```sh
python3 gen.py 1000000000   # writes input + golden output
./bench.sh -t 12 5          # rebuilds from scratch, min-of-5, pins P-cores first
```

Output is checked against the golden file by SHA-256 on every run.
`./bench.sh [-t threads] [runs] [-- cmake-args]`, e.g. `-- -DCONTAINER_TYPE=1` to race absl.
