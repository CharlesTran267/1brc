#!/bin/sh
# Benchmark the solver. Reports min-of-N of its own "Total solve" timer:
# interference is one-sided, so the minimum is the least-contaminated estimate.
#
# Pinned to CPUs 4-11 = the 8 E-cores. Homogeneous and no SMT siblings, so every
# thread does comparable work. CPUs 0-3 are 2 HT P-cores at a different clock and
# IPC; mixing core types is what makes the per-thread times spread ~40%.
# Usage: ./bench.sh [runs]      PERF=1 ./bench.sh   also dumps counters
set -e
N=${1:-3}

# rebuild first: benching a stale binary is the easiest way to draw a wrong conclusion.
# not piped, so `set -e` still catches a compile error.
cmake --build build -j"$(nproc)"

CPUS=${CPUS:-4-11}
BIN=./build/charles_1brc
TAG=1e$(sed -n 's/^#define SIZE_EXP \([0-9]*\).*/\1/p' src/main.cpp)

cat "input/measurements_$TAG.txt" > /dev/null   # warm page cache; cold-read noise dwarfs everything else
echo "$TAG, cpus $CPUS, $N runs"

best=""
i=1
while [ "$i" -le "$N" ]; do
  ms=$(taskset -c "$CPUS" "$BIN" | sed -n 's/^Total solve:\([0-9.]*\) ms/\1/p')
  echo "  run $i: $ms ms"
  best=$(printf '%s\n%s\n' "$best" "$ms" | grep . | sort -n | head -1)
  i=$((i + 1))
done
echo "best: $best ms"

if [ -n "$PERF" ]; then
  perf stat -e task-clock,cycles,instructions,cache-misses \
    taskset -c "$CPUS" "$BIN" > /dev/null
fi
