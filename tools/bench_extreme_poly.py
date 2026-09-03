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
# LONGEST FIRST. "hierarchy + layers" has to be tried before "hierarchy" or the
# layered rows fall through to the previous representation and overwrite its
# entries under the same key -- silently, because every field still parses.
REPRESENTATION = re.compile(
    r"^\s+(hierarchy \+ layers|fixed mesh|adaptive surface|hierarchy):")
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


def arguments():
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
    return ap.parse_args()


def measure_rows(binary, sizes, args):
    """One row per size, each with the load either side of it recorded.

    Returns (rows, load, raw output, notes). A row is never silently dropped: a
    reader deciding whether to trust a number needs to see that it was taken
    and why it is suspect, which is what `notes` carries.
    """
    rows, load, raw, notes = {}, {}, [], []
    for size in sizes:
        need, have = predicted_mb(size), free_mb()
        if need > have:
            print(f"== {size} vertices: SKIPPED, predicts ~{need} MB and {have} MB is available")
            notes.append(f"{size}: skipped, not enough memory ({need} MB predicted, "
                         f"{have} MB available)")
            continue
        print(f"== {size} vertices: predicts ~{need} MB, {have} MB available", flush=True)
        text, code, before, after = run_size(binary, size, args.footprints, args.reps,
                                             args.which, args.levels)
        raw.append(text)
        moved = abs(after - before)
        flag = "" if moved <= args.load_tolerance else "  ** LOAD MOVED **"
        print(f"   load {before:.2f} -> {after:.2f}{flag}", flush=True)
        if code != 0:
            notes.append(f"{size}: benchmark exited {code}")
            continue
        if moved > args.load_tolerance:
            notes.append(f"{size}: load moved {before:.2f} -> {after:.2f}; "
                         f"re-run before quoting this row")
        rows[size] = parse(text)
        load[size] = (before + after) / 2.0
    return rows, load, raw, notes


def load_notes(load, tolerance):
    """The check the within-row one cannot make.

    A row taken under a load that held steady at 11.6 for its whole duration
    passes the movement check, and a ratio between it and a row taken under 7 is
    still noise presented as a finding. It was not hypothetical: the 5M row of
    the first full matrix read 2.6x-8.5x against 100k while the 10M and 20M rows
    read 1.3x-2.7x, which is the model getting CHEAPER as it grows — arithmetic
    nothing about the engine can produce. The load level was the difference, and
    nothing in the output said so.
    """
    if len(load) < 2:
        return []
    lo, hi = min(load.values()), max(load.values())
    if hi - lo <= tolerance:
        return []
    trail = ", ".join(f"{size}: {value:.2f}" for size, value in sorted(load.items()))
    return [f"the load LEVEL differs by {hi - lo:.2f} across rows ({trail}); rows taken "
            f"under different loads are not comparable to each other however steady each "
            f"one was, and the ratios below span them"]


def report_ratios(rows, measured):
    """Every stage of every footprint against the smallest size that HAS it.

    THE BASELINE IS PER KEY, and the reason is 7.1's own matrix rather than a
    refinement. A footprint that does not fit inside a model is skipped, so the
    500k footprint first exists at 5M vertices and the 100k one at 1M. A single
    baseline taken from the smallest size measured therefore has neither key,
    and a report that skips what its baseline lacks drops the two LARGEST
    footprints the requirement names — measured by the binary, printed in the
    raw output, and absent from the only table anybody reads. Each row now says
    which size it is against, because a ratio whose baseline is not stated is
    not a number.
    """
    print("\n# RATIOS against the smallest size that measured that footprint (named per row),")
    print("# same footprint, same stage. A stage that is O(model) reads as the model ratio;")
    print("# one that costs what it touches reads as about 1.")
    keys = sorted({k for row in rows.values() for k in row})
    for key in keys:
        present = [size for size in measured if key in rows[size]]
        if len(present) < 2:
            representation, footprint, stage = key
            print(f"  {representation:18s} fp {footprint:7d}  {stage:14s} | "
                  f"only {present[0] / 1e6:.1f}M measured it; no ratio")
            continue
        base = present[0]
        b50, b95 = rows[base][key][0:2]
        cells = [_ratio_cell(rows[size].get(key), size, b50, b95) for size in present[1:]]
        representation, footprint, stage = key
        print(f"  {representation:18s} fp {footprint:7d}  {stage:14s} | "
              f"vs {base / 1e6:5.1f}M | " + " | ".join(cells))


def _ratio_cell(cell, size, b50, b95):
    if cell is None:
        return f"{size / 1e6:.0f}M    -   "
    return f"{size / 1e6:.0f}M p50 {cell[0] / b50:5.2f}x p95 {cell[1] / b95:5.2f}x"


def main() -> int:
    args = arguments()
    binary = REPO / args.build / "bench_extreme_poly"
    if not binary.exists():
        print(f"no benchmark at {binary}; configure with -DCLAY_BUILD_BENCHMARKS=ON",
              file=sys.stderr)
        return 2

    sizes = [int(s) for s in args.sizes.split(",")]
    rows, load, raw, notes = measure_rows(binary, sizes, args)
    notes += load_notes(load, args.load_tolerance)
    if args.out:
        pathlib.Path(args.out).write_text("\n".join(raw))

    measured = [s for s in sizes if s in rows]
    if len(measured) < 2:
        print("\nfewer than two rows measured; there is nothing to form a ratio from")
        return 0
    report_ratios(rows, measured)

    if notes:
        print("\n# NOT COMPARABLE, and said so rather than dropped:")
        for note in notes:
            print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
