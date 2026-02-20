#!/bin/sh
# Benchmark the solver. Reports min-of-N of its own "Total solve" timer:
# interference is one-sided, so the minimum is the least-contaminated estimate.
#
# Pins exactly one CPU per thread, fastest first: P-cores (one SMT thread per
# physical core), then E-cores, then leftover SMT siblings. Override with CPUS=...
# Caveat: past 2 threads the pool mixes P and E cores, which spreads per-thread
# times ~40%; the static split makes the whole run wait on the slowest core.
# Usage: ./bench.sh [-t threads] [runs]      PERF=1 ./bench.sh   also dumps counters
set -e

T=""
while getopts t: opt; do
  case $opt in
    t) T=$OPTARG ;;
    *) echo "usage: $0 [-t threads] [runs]" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
N=${1:-3}

# Own build dir, wiped every run: never inherits a stale cache (a Debug build/
# or a leftover NUM_THREADS), and a compile error stops the bench via set -e.
BDIR=build-bench
rm -rf "$BDIR"
cmake -B "$BDIR" -DCMAKE_BUILD_TYPE=Release ${T:+-DNUM_THREADS="$T"} -DDEBUG=0> /dev/null
cmake --build "$BDIR" -j"$(nproc)"

# one line per logical cpu: "<cpu> <core_id> <max_freq>"; pick one cpu per
# physical core in descending freq, append SMT siblings last, take first T.
if [ -z "$CPUS" ]; then
  CPUS=$(for c in /sys/devices/system/cpu/cpu[0-9]*; do
      echo "${c##*/cpu} $(cat "$c/topology/core_id") $(cat "$c/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo 0)"
    done | sort -k3,3nr -k1,1n \
    | awk -v n="${T:-8}" '
        !seen[$2]++ { pri = pri " " $1; next }
                    { smt = smt " " $1 }
        END {
          split(pri smt, cpu, " ")
          for (i = 1; i <= n && i in cpu; i++) out = out (i > 1 ? "," : "") cpu[i]
          print out
        }')
fi
BIN=./$BDIR/charles_1brc
TAG=1e$(sed -n 's/^#define SIZE_EXP \([0-9]*\).*/\1/p' src/main.cpp)

cat "input/measurements_$TAG.txt" > /dev/null   # warm page cache; cold-read noise dwarfs everything else
echo "$TAG, cpus $CPUS, threads ${T:-8}, $N runs"

best=""
i=1
while [ "$i" -le "$N" ]; do
  ms=$(taskset -c "$CPUS" "$BIN" | sed -n 's/^Total time:\([0-9.]*\) ms/\1/p')
  echo "  run $i: $ms ms"
  best=$(printf '%s\n%s\n' "$best" "$ms" | grep . | sort -n | head -1)
  i=$((i + 1))
done
echo "best: $best ms"

if [ -n "$PERF" ]; then
  perf stat -e task-clock,cycles,instructions,cache-misses \
    taskset -c "$CPUS" "$BIN" > /dev/null
fi
