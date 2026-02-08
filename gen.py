#!/usr/bin/env python3
"""Generate the measurements input + golden output the solver expects.

Usage: gen.py [rows] [indir] [outdir]   (defaults: 100000000 input output)

Files are tagged by row count: input/measurements_1e6.txt, output/golden_output_1e6.txt.
Keep src/main.cpp's SIZE_EXP in sync.

Golden is computed from exact integer tenths, then formatted the same way
main.cpp does ("{name=min/mean/max, ...}\n", %.1f), so the sha256 compare
in main() is meaningful.
"""
import random
import sys

import numpy as np

ROWS = int(sys.argv[1]) if len(sys.argv) > 1 else 100_000_000
INDIR = sys.argv[2] if len(sys.argv) > 2 else "input"
OUTDIR = sys.argv[3] if len(sys.argv) > 3 else "output"
# a station's mean lands on a .x5 tie with probability ~1/rows_per_station, so keep
# stations near sqrt(ROWS) to hold expected ties at ~1 (10_000 at the full 1e8)
NS = min(10_000, max(1, int(ROWS**0.5)))
TENTHS = 1999      # -99.9 .. 99.9
CHUNK = 1_000_000

exp = len(str(ROWS)) - 1
TAG = f"1e{exp}" if ROWS == 10**exp else str(ROWS)

with open("1brc/data/weather_stations.csv", encoding="utf-8") as f:
    all_names = sorted({l.split(";")[0] for l in f if l.strip() and not l.startswith("#")})
random.seed(42)
names = random.sample(all_names, NS)
val_str = [f"{t / 10:.1f}" for t in range(-999, 1000)]

rng = np.random.default_rng(42)
hist = np.zeros(NS * TENTHS, dtype=np.int64)

with open(f"{INDIR}/measurements_{TAG}.txt", "w", encoding="utf-8") as f:
    for done in range(0, ROWS, CHUNK):
        n = min(CHUNK, ROWS - done)
        si = rng.integers(0, NS, n)
        ti = rng.integers(0, TENTHS, n)
        f.write("\n".join([names[a] + ";" + val_str[b] for a, b in zip(si.tolist(), ti.tolist())]))
        f.write("\n")
        hist += np.bincount(si * TENTHS + ti, minlength=NS * TENTHS)
        print(f"\r{done + n}/{ROWS}", end="", flush=True)
print()

hist = hist.reshape(NS, TENTHS)
count = hist.sum(1)
total = hist @ np.arange(-999, 1000)  # sum in tenths, exact

parts, ties = [], []
for i in sorted(range(NS), key=lambda i: names[i].encode()):  # std::map<string_view> byte order
    c = int(count[i])
    if not c:
        continue
    t = int(total[i])
    # exact mean lands on a .x5 boundary -> printf rounding depends on fp noise, so the
    # solver's threaded double sum may disagree with this golden on that station
    if 2 * t % c == 0 and (2 * t // c) % 2:
        ties.append(names[i])
    nz = np.nonzero(hist[i])[0]
    lo, hi = (nz[0] - 999) / 10, (nz[-1] - 999) / 10
    parts.append(f"{names[i]}={lo:.1f}/{t / (10 * c):.1f}/{hi:.1f}")
if ties:
    print(f"warning: {len(ties)} station(s) with a .x5 mean tie: {ties[:5]}")

with open(f"{OUTDIR}/golden_output_{TAG}.txt", "w", encoding="utf-8") as f:
    f.write("{" + ", ".join(parts) + "}\n")
print(f"{len(parts)} stations -> {OUTDIR}/golden_output_{TAG}.txt")
