#!/usr/bin/env python3
"""Benchmark regression gate (build-packaging spec).

Reads google-benchmark JSON and enforces generous floor thresholds —
deliberately loose so shared CI runners don't flake, tight enough to catch
order-of-magnitude regressions (an accidental O(n^2), a dropped thread pool,
a debug-mode kernel). Report includes baseline and measured values.
"""

import json
import sys

# floors: items/sec minimums, time maximums (ms)
FLOORS = {
    "BM_EvalPoints": {"min_items_per_second": 500_000},
    "BM_BrickFill": {"min_items_per_second": 100},
    "BM_MeshTape": {"max_ms": 20_000},
    "BM_SurfaceNets": {"max_ms": 20_000},
}

# relative gates: (bench, must_be_faster_than) — meshing spec requires the
# surface-nets preview to beat the marching mesher at equal resolution, and
# brick-cache requires that meshing a dab's worth of bricks costs less than
# meshing the surface, which is the whole point of taking a key list
FASTER_THAN = [
    ("BM_SurfaceNets", "BM_MeshTape"),
    ("BM_MeshBricksSubset", "BM_MeshBricksWhole"),
]

# ratio gates: (bench, reference, max_ratio) — bench must cost at most
# max_ratio times the reference. Issue #73: gradient normals over a FIXED
# brick set must not scale with the total document, so meshing the same 80
# bricks against a 193-node document is gated against the 1-node document.
# Before the per-brick culled attribute pass the ratio was ~18x; after, the
# grown document pays only the per-brick cull tests. 3x is the same generous
# style as the floors: it catches the O(document) path coming back, not noise.
MAX_RATIO = [
    ("BM_MeshBricksGradGrownDoc", "BM_MeshBricksGradFreshDoc", 3.0),
    # Cull index (accel/cull-index): refilling a fixed dab's bricks against a
    # grown document is gated against the fresh one. Before the per-revision
    # CullIndex + per-batch CullPlan every per-brick compile walked the whole
    # document, so this ratio scaled with total nodes; after, the grown
    # document pays one cached-bounds pass plus the coarse cull. Same 3x
    # generosity as above: it catches the O(document x bricks) walk coming
    # back, not runner noise.
    ("BM_DabRefillGrownDoc", "BM_DabRefillFreshDoc", 3.0),
]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_bench.py <benchmark.json>", file=sys.stderr)
        return 2
    with open(sys.argv[1]) as f:
        data = json.load(f)
    failures = []
    seen = set()
    times = {}
    for bench in data.get("benchmarks", []):
        name = bench["name"].split("/")[0]
        seen.add(name)
        times[name] = bench.get("real_time", 0)
        if name not in FLOORS:
            continue
        rule = FLOORS[name]
        if "min_items_per_second" in rule:
            ips = bench.get("items_per_second", 0)
            print(f"bench-gate: {name}: {ips:,.0f} items/s (floor {rule['min_items_per_second']:,})")
            if ips < rule["min_items_per_second"]:
                failures.append(f"{name}: {ips:,.0f} items/s below floor")
        if "max_ms" in rule:
            ms = bench.get("real_time", 0)
            print(f"bench-gate: {name}: {ms:,.1f} ms (ceiling {rule['max_ms']:,} ms)")
            if ms > rule["max_ms"]:
                failures.append(f"{name}: {ms:,.1f} ms above ceiling")
    for name in FLOORS:
        if name not in seen:
            failures.append(f"{name}: benchmark missing from results")
    for fast, slow in FASTER_THAN:
        if fast in times and slow in times:
            print(f"bench-gate: {fast}: {times[fast]:,.1f} ms vs {slow}: {times[slow]:,.1f} ms")
            if times[fast] >= times[slow]:
                failures.append(f"{fast}: {times[fast]:,.1f} ms not faster than {slow}")
    for name, reference, max_ratio in MAX_RATIO:
        for missing in (n for n in (name, reference) if n not in seen):
            failures.append(f"{missing}: benchmark missing from results")
        if name not in times or reference not in times or times[reference] <= 0:
            continue
        ratio = times[name] / times[reference]
        print(f"bench-gate: {name}: {times[name]:,.1f} ms is {ratio:.2f}x "
              f"{reference}: {times[reference]:,.1f} ms (ceiling {max_ratio}x)")
        if ratio > max_ratio:
            failures.append(f"{name}: {ratio:.2f}x {reference} above the {max_ratio}x ceiling")
    for f_ in failures:
        print(f"bench-gate: FAIL {f_}", file=sys.stderr)
    if not failures:
        print("bench-gate: OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
