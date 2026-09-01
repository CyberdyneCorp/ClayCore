#!/usr/bin/env python3
"""Drive `bench_extreme_poly` under the shared-box discipline and report ratios.

    tools/bench_extreme_poly.py --build build/cpu-only
    tools/bench_extreme_poly.py --sizes 100000,1000000,5000000,10000000,20000000

WHY THIS EXISTS RATHER THAN JUST RUNNING THE BINARY.

A millisecond on this machine is a fact about this machine. The number that
travels is the RATIO of the same footprint at two model sizes, because the
locality requirement is stated as a ratio: sixteen times the vertices at the
same touched region must not be sixteen times the dab. So this reads the
benchmark's per-stage percentiles, takes the smallest size as the baseline, and
prints what every larger size costs against it.

AND BECAUSE THE BOX IS SHARED. Three agents build on this machine and it picks
up unrelated jobs mid-run, so a row measured under load 4 and a row measured
under load 22 are not comparable and the ratio between them is noise presented
as a finding. Every row therefore records `uptime`'s one-minute load average
BEFORE and AFTER it, and a row whose load moved by more than `--load-tolerance`
is flagged in the output — not silently dropped, because a reader deciding
whether to trust a number needs to see that it was taken and why it is suspect.

WHAT A ROW COSTS is printed with it, because the 10M and 20M rows are the
expensive ones: the benchmark reports its own setup time and resident set per
representation, and this driver refuses to start a row whose predicted resident
set exceeds the free memory it can see. A benchmark that gets the process killed
measures nothing and takes the machine's other tenants with it.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

STAGE = re.compile(
    r"^\s+(\S[\S ]*?)\s+p50\s+([\d.]+)\s+p95\s+([\d.]+)\s+p99\s+([\d.]+)\s+max\s+([\d.]+)"
    r"\s+mean\s+([\d.]+) us")
FOOTPRINT = re.compile(r"^\s+footprint\s+(\d+)")
REPRESENTATION = re.compile(r"^\s+(fixed mesh|adaptive surface|hierarchy):")
SIZE = re.compile(r"^== (\d+) vertices")


def load_average() -> float:
    """The one-minute load average, which is what `uptime` reports first."""
    return os.getloadavg()[0]


def free_mb() -> int:
    """MemAvailable, which is what a large allocation can actually get."""
    try:
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 0


def predicted_mb(vertices: int) -> int:
    """Roughly what the fixed-mesh row of this size will hold.

    Measured rather than derived: the 1M row reports 357 MB resident, which is
    about 357 bytes per vertex once the mesh, the chunk table, the adjacency and
    the ray tree are all up. Doubled as the margin, because the adaptive and
    multires rows of the same size are larger and because a prediction that is
    optimistic here gets the process killed.
    """
    return int(vertices * 357 * 2 / 1024 / 1024)


def run_size(binary: pathlib.Path, size: int, footprints: str, reps: int, which: str,
             levels: int) -> tuple:
    before = load_average()
    proc = subprocess.run(
        [str(binary), f"--sizes={size}", f"--footprints={footprints}", f"--reps={reps}",
         f"--which={which}", f"--levels={levels}"],
        capture_output=True, text=True)
    after = load_average()
    return proc.stdout, proc.returncode, before, after


def parse(text: str) -> dict:
    """{(representation, footprint, stage): (p50, p95, p99, max, mean)}"""
    out = {}
    representation, footprint = "?", 0
    for line in text.splitlines():
        m = REPRESENTATION.match(line)
        if m:
            representation = m.group(1)
            continue
        m = FOOTPRINT.match(line)
        if m:
            footprint = int(m.group(1))
            continue
        m = STAGE.match(line)
        if m:
            out[(representation, footprint, m.group(1).strip())] = tuple(
                float(m.group(i)) for i in range(2, 7))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build/cpu-only")
    ap.add_argument("--sizes", default="100000,1000000,5000000,10000000,20000000")
    ap.add_argument("--footprints", default="1000,5000,20000,100000,500000")
    ap.add_argument("--reps", type=int, default=30)
    ap.add_argument("--which", default="fixed")
    ap.add_argument("--levels", type=int, default=3)
    ap.add_argument("--load-tolerance", type=float, default=4.0,
                    help="how far the one-minute load may move across a row before it is "
                         "flagged as not comparable")
    ap.add_argument("--out", default=None, help="write the raw benchmark output here too")
    args = ap.parse_args()

    binary = REPO / args.build / "bench_extreme_poly"
    if not binary.exists():
        print(f"no benchmark at {binary}; configure with -DCLAY_BUILD_BENCHMARKS=ON",
              file=sys.stderr)
        return 2

    sizes = [int(s) for s in args.sizes.split(",")]
    rows, raw, flagged = {}, [], []
    for size in sizes:
        need, have = predicted_mb(size), free_mb()
        if need > have:
            print(f"== {size} vertices: SKIPPED, predicts ~{need} MB and {have} MB is available")
            flagged.append(f"{size}: skipped, not enough memory ({need} MB predicted, "
                           f"{have} MB available)")
            continue
        print(f"== {size} vertices: predicts ~{need} MB, {have} MB available", flush=True)
        text, code, before, after = run_size(binary, size, args.footprints, args.reps,
                                             args.which, args.levels)
        raw.append(text)
        moved = abs(after - before)
        note = "" if moved <= args.load_tolerance else "  ** LOAD MOVED **"
        print(f"   load {before:.2f} -> {after:.2f}{note}", flush=True)
        if code != 0:
            flagged.append(f"{size}: benchmark exited {code}")
            continue
        if moved > args.load_tolerance:
            flagged.append(f"{size}: load moved {before:.2f} -> {after:.2f}; "
                           f"re-run before quoting this row")
        rows[size] = parse(text)

    if args.out:
        pathlib.Path(args.out).write_text("\n".join(raw))

    measured = [s for s in sizes if s in rows]
    if len(measured) < 2:
        print("\nfewer than two rows measured; there is nothing to form a ratio from")
        return 0

    base = measured[0]
    print(f"\n# RATIOS against the {base}-vertex row, same footprint, same stage.")
    print("# A stage that is O(model) reads as the model ratio; one that costs what it")
    print("# touches reads as about 1.")
    keys = sorted({k for row in rows.values() for k in row})
    for representation, footprint, stage in keys:
        if (representation, footprint, stage) not in rows[base]:
            continue
        b50, b95 = rows[base][(representation, footprint, stage)][0:2]
        cells = []
        for size in measured[1:]:
            cell = rows[size].get((representation, footprint, stage))
            if cell is None:
                cells.append(f"{size / 1e6:.0f}M    -   ")
                continue
            cells.append(f"{size / 1e6:.0f}M p50 {cell[0] / b50:5.2f}x p95 {cell[1] / b95:5.2f}x")
        print(f"  {representation:18s} fp {footprint:7d}  {stage:14s} | " + " | ".join(cells))

    if flagged:
        print("\n# NOT COMPARABLE, and said so rather than dropped:")
        for note in flagged:
            print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
