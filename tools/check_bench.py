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
    # Issue #86: voxel greedy meshing must cost the material, not a chunk-map
    # lookup per cell probed. Both grids hold one voxel per chunk. Measured at
    # 3.8 ms and 256 ms with the per-cell lookup, 0.21 ms and 12.9 ms without,
    # on a Linux desktop; the ceilings sit between, closer to the fast side,
    # which is the same generosity as the ceilings above. A ratio against a
    # solid chunk would be the wrong gate — see the note on the benchmarks.
    "BM_VoxelMeshSparseChunk": {"max_ms": 2},
    "BM_VoxelMeshSparse64Chunks": {"max_ms": 120},
    # Part 2 of the same issue: two chunks of that 64-chunk grid, meshed
    # through the regional call. It is the bench above divided by 32 — 0.40 ms
    # against 12.9 ms here — so the ceiling gates the SHAPE. A regression that
    # meshed the whole grid for a two-chunk request would land at 12.9 ms and
    # blow this by 30x, which is the failure worth catching.
    "BM_VoxelMeshDirtyChunks": {"max_ms": 4},
}

# relative gates: (bench, must_be_faster_than) — meshing spec requires the
# surface-nets preview to beat the marching mesher at equal resolution, and
# brick-cache requires that meshing a dab's worth of bricks costs less than
# meshing the surface, which is the whole point of taking a key list
FASTER_THAN = [
    ("BM_SurfaceNets", "BM_MeshTape"),
    ("BM_MeshBricksSubset", "BM_MeshBricksWhole"),
    # Grid-path consolidation (accel/parallel-consolidate): baking a grown
    # layer through the CPU backend's batch path must beat the serial
    # one-point-at-a-time bake it replaced (kept in the benchmark as the
    # reference; the two are byte-identical by contract). Catches the bake
    # falling off the pool; holds on any machine with more than one core,
    # since both sides pay the same serial redistance floor (4.7x on an
    # M2 Max).
    ("BM_ConsolidateGrownDoc", "BM_ConsolidateSerialGrownDoc"),
    # And the colour pass, which the pair above cannot see: it compares the
    # pooled bake against the serial reference, and when the bake started
    # filling a colour channel BOTH sides of that comparison moved, so a 1.5x
    # regression on the reference iPad still read as a pass here. A one-colour
    # layer must be FASTER than a two-colour one over the same document,
    # because the second evaluation of the tape is exactly the difference
    # between them. Catches a uniform layer paying for colour it cannot show.
    ("BM_ConsolidateGrownDoc", "BM_ConsolidateColoredGrownDoc"),
    # Batched brick raycast (accel/parallel-raycast): the batched C-ABI call
    # fans its rays across the CPU backend's pool and must beat the same rays
    # issued one single-ray call at a time (kept in the benchmark as the
    # reference; slot-for-slot bit-identical by contract). Catches the batch
    # falling off the pool; holds on any machine with more than one core,
    # since both sides pay the identical per-ray march (8x on an M2 Max).
    ("BM_RaycastBricksBatch", "BM_RaycastBricksSerial"),
    # Resident uploaded tapes (accel/metal-persistent): re-evaluating one
    # compiled tape must beat alternating more tapes than the Metal backend's
    # residency holds — the two run the identical dispatch and differ only in
    # whether the tape (a consolidated volume, megabytes of blob) is
    # re-uploaded per call. Catches the residency falling out. The pair is
    # registered only on machines with a Metal device; FASTER_THAN skips
    # missing names, so CPU-only CI is unaffected (3.8x on an M2 Max).
    ("BM_MetalTapeResident", "BM_MetalTapeReupload"),
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
    # Batched brick-mesh attributes (accel/shared-attribute-tape): on a
    # densely sculpted region the attribute pass is held to a bounded
    # multiple of refilling the same bricks against the same document. Both
    # evaluate similar point counts against the same long culled tapes
    # through the CPU backend's pool, so the healthy ratio is a few x (5.3x
    # on an M2 Max) and stays put; the attribute taps falling back to one
    # vertex at a time on one core measured 14x there. 10x catches the
    # serial path coming back on capable machines without flaking on small
    # runners, where both sides lose the pool together.
    ("BM_MeshBricksGradDenseDoc", "BM_DabRefillDenseDoc", 10.0),
    # Undo of a stamp stroke (accel/undo-removal): undoing 100 stamps on a
    # 10k-stamp document is gated against the same stroke on a 100-stamp one.
    # Before the location index in SdfContent every removed stamp's locate()
    # walked the whole node arena, so this ratio scaled with document size
    # (33x between these sizes); after, both sides pay the same per-stamp
    # hash work. 3x catches the O(document) walk coming back, not noise.
    ("BM_UndoStampsGrownDoc", "BM_UndoStampsFreshDoc", 3.0),
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
