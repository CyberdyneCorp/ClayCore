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
    # Subdividing a whole level. #134 gave the level stack region-refinement,
    # and the per-child chunk_key() its refinement test needs is dead work on a
    # whole level — 2.36x on the device gate (voxel_add_level, 0.51 -> 1.21 ms),
    # 1.84x on this benchmark (0.46 -> 0.84 ms on an M-series Mac).
    #
    # This ceiling does NOT catch that. 1.84x is inside the spread between this
    # machine and a shared runner, so a threshold tight enough to fail on it
    # would flake on CI instead. It is set for the order-of-magnitude case, per
    # this file's own docstring, and the DEVICE GATE is what holds the tighter
    # line — it is what found this one.
    #
    # RAISED 2.5 -> 16.0 when the spread was fixed (#196), and the reason is
    # worth keeping: the ceiling was not too tight, the WORKLOAD was too small.
    # The case scatters 400 blobs of 64 cells and should occupy ~25,600 cells;
    # the old golden-ratio walk collapsed them onto the plane x+y=0, where they
    # landed on top of each other and occupied 3,492. It measured a seventh of
    # the work it claimed. Corrected, the same case is 0.85 -> 5.31 ms on this
    # machine, which is the cost it always should have had, so the ceiling moves
    # with it rather than the workload shrinking to fit the old number. The
    # headroom ratio is unchanged: ~3x measured, as before.
    "BM_VoxelAddLevelWhole": {"max_ms": 16.0},
    # Writing INTO a whole level stack — the same defect #137 hoisted out of
    # subdivide_into, in the two places propagation reaches it: record_detail
    # (via refresh_detail, per child of every downward step) and propagate_up
    # (per child, recursing per level). Both constant-true on a whole level,
    # both costing a chunk_key() to reach.
    #
    # It is a SMALLER fraction here than on add_level, and the ceiling says so
    # rather than implying a gate it cannot be: 0.232 -> 0.211 ms measured back
    # to back at 7 repetitions with ASLR off, so 1.10x. subdivide_into's inner
    # loop is almost nothing BUT the key; a write is dominated by write_cell's
    # own hash and chunk lookup, which the key is a tenth of.
    #
    # No threshold catches 1.10x without flaking on a shared runner. This is
    # here to put the number in CI output — no other benchmark covers writing
    # under a level stack at all — and the DEVICE gate holds the tighter line.
    "BM_VoxelWriteUnderLevels": {"max_ms": 1.5},
    # A large-radius voxel verb — the device gate's own voxel_smooth_r32 size,
    # measured here on a machine with a fan. 1.15 -> 0.14 ms (8.1x) when the
    # snapshot stopped hashing per cell and the decide pass went on the pool.
    #
    # The ceiling is set where a REGRESSION of the snapshot fix would trip it:
    # that half alone is 3.45x, far outside runner spread, unlike the threading
    # half which is not gateable here for the usual reason.
    "BM_VoxelSculptSmoothR32": {"max_ms": 0.9},
    # Rasterizing a document into cells — a tape evaluation per cell, the case
    # #119 correctly predicted was parallel (unlike the verbs, where the
    # snapshot turned out to dominate). 42.1 -> 7.0 ms, 6.0x.
    #
    # The ceiling is set from the RUNNER, not from a development machine, and
    # the first attempt got that wrong: 15 ms was twice the 7.0 ms measured
    # locally, and CI runs this at 21.3 ms. The runner is ~3x slower — which is
    # the spread every other note in this file warns about and this one talked
    # itself out of.
    #
    # 60 ms is ~2.8x the observed runner figure and well under the ~128 ms the
    # serial path would take there, so it still catches the split being lost.
    "BM_VoxelRasterizeTape": {"max_ms": 60},
    # Importing a mesh as a field — the first thing that happens to every model
    # a host loads, and it was 4.8 SECONDS for a 9k-triangle model at a 0.01
    # cell. A BVH signed-distance query with a generalized winding number per
    # sample, which is pure: 4767 -> 543 ms on the pool, 8.8x.
    #
    # Set from the RUNNER: it is ~3x slower than a development machine (see the
    # rasterize note above, where a ceiling taken from local numbers failed CI),
    # so 543 ms here is ~1.6 s there. 5000 leaves room over that and is far
    # under the ~14 s the serial path would take, which is what it catches.
    "BM_MeshToField": {"max_ms": 5000},
    # Meshing a whole document over a lattice — the export path. 1062 -> 668 ms
    # (1.59x) with the marching-tets pass on the pool.
    #
    # A modest ratio ON PURPOSE, and the ceiling says why rather than implying
    # the split underperformed: the field evaluation was already parallel
    # (eval_grid is a CPU batch path) and the WELD is still serial — one
    # Builder deduping 1.2M triangles' vertices, which is what makes the seams
    # exact. Amdahl, not a defect. Set from the runner at ~3x.
    "BM_MeshTapeWholeDoc": {"max_ms": 4000},
    # The deep edit list (#118 workstream C): 8 bricks refilled against a
    # 2,000-item document.
    #
    # The gated one is the PLANNED path, because that is what a host pays:
    # clay_brick_cache_eval_requests builds a CullIndex per revision and a
    # CullPlan per batch. 0.321 ms, of which 0.164 is the cull.
    #
    # The unplanned one is gated too, as the contrast: 1.30 ms for the same
    # work without the index. The first version of this benchmark measured only
    # that and reported it as the cost of a dab, which overstated it 4x.
    #
    # What survives the correction is the SLOPE: the cull is linear in item
    # count on both paths (0.019 -> 0.178 ms planned, over 10x the items), so
    # the index lowered the constant and left the shape.
    "BM_DeepDocRefillPlanned2000": {"max_ms": 4},
    # 10 000 items, which is where add-item-spatial-index's argument lives. The
    # extrapolation above is now MEASURED rather than estimated, and it lands
    # where the estimate said: 0.926 ms of culling, 0.934 ms for the whole dab.
    #
    # That matters because the proposal was written before CullIndex and
    # CullPlan existed and argues from ~15 ms of culling at this size. The
    # index is 16x off that, and a dab at 10 000 items sits inside a 4.17 ms
    # frame share rather than blowing it before evaluation starts.
    #
    # The slope is still real and still linear, and culling is still 99% of the
    # dab's cost here (0.926 of 0.934) — so the spatial index is still the right
    # next move on this path. What changed is the urgency, not the direction.
    #
    # Ceilings are ~6x the local number because the CI runner measures about 3x
    # slower and a benchmark gate that flakes gets ignored.
    "BM_DeepDocCullPlanned10000": {"max_ms": 6},
    "BM_DeepDocRefillPlanned10000": {"max_ms": 6},
    "BM_DeepDocRefill2000": {"max_ms": 12},
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
    # ("BM_SurfaceNets", "BM_MeshTape") WAS here, for the meshing spec's
    # "preview is cheaper than marching" claim. It was measuring the ATTRIBUTE
    # PASS, not the mesher. Both sides walked the tape once per vertex for the
    # colour and four more for the gradient, on one thread, and surface nets
    # emits 3.2x fewer vertices — so the pair passed on vertex count and the
    # geometry step never entered into it. Batching that pass (#302) removed
    # the confound and left the meshers visible:
    #
    #   bench_document, voxel 0.02      marching   surface nets
    #     geometry only                   82.5 ms      138.3 ms   nets 1.68x SLOWER
    #     + attributes, before #302       500.7 ms     275.0 ms   nets 1.8x faster
    #     + attributes, after #302         91.0 ms     144.4 ms   nets 1.59x slower
    #
    # So the claim is false on the tape path and the pair cannot be made to
    # hold by re-scoping it. Removed rather than quietly weakened, and #304
    # carries the numbers and the question of what the spec should say. What
    # nets IS cheaper at — 3.2x the triangles for a preview — is not a timing
    # gate and has none here yet.
    # The non-brick meshers' attribute pass (#302): `apply_tape_attributes`
    # walked the tape once per vertex for the colour and four more for the
    # gradient, on one thread, while the brick mesher had gone through the
    # backend's batch since #73. Both sides here evaluate the identical taps
    # against the identical tape over the identical vertices and are
    # byte-identical by contract; they differ only in whether the evaluation is
    # batched. Catches the pass falling back to the serial walk — which it does
    # silently, by design, when no CPU backend is registered.
    ("BM_MeshTapeAttributes", "BM_MeshTapeAttributesSerial"),
    # Continuing the fold against replaying it (#306): a dab re-evaluated every
    # surviving item of the edit list over its samples, so its cost followed
    # what the artist had already sculpted rather than what the dab added. The
    # suffix is two instructions whatever the document holds, so this pair holds
    # a SCALING LAW rather than a percentage and its margin widens with the
    # fixture -- 76x at 5,000 dabs, 845x at 50,000. Both sides evaluate the same
    # points and are bit-identical by contract (test_suffix_tape.cpp).
    ("BM_DabSuffixSeeded", "BM_DabFullWalk"),
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
    # The VOLUME bake the document-sourced verbs reach — clay_item_volume_from_document
    # and the _relax_from / _flatten_from pair, plus their pyclay equivalents.
    # bake_layer was gated by the pair above and these three were not, which is
    # how they kept the one-point-at-a-time walk long after bake_layer stopped
    # using it: 386 ms against 23.6 ms on a twelve-core machine, byte-identical
    # output. This pair is what stops that happening again on THIS path — it
    # measures the bake alone, with no redistance floor on either side, so it is
    # a wider margin than the consolidate pair and a sharper signal.
    ("BM_VolumeBakeDoc", "BM_VolumeBakeSerialDoc"),
    # And the third document-sourced verb, which needed a different kind of
    # batched source: move_topological's query positions are the PULLED-BACK
    # points rather than the sample lattice, so it takes a batch of arbitrary
    # points. 15x on a twelve-core machine, byte-identical output.
    ("BM_VolumeMoveDoc", "BM_VolumeMoveSerialDoc"),
    # And the bake with its tape culled per brick, against the whole-tape bake
    # it equals byte for byte. The decision to cull is ADAPTIVE — a wide enough
    # blend keeps every item in every brick's tape and culling becomes pure
    # overhead, so the fill measures a sample of the lattice and falls back —
    # and this pair is the only thing that would notice the guard deciding never
    # to cull at all.
    ("BM_VolumeBakeCulledDoc", "BM_VolumeBakeWholeTapeDoc"),
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
    # BVH refit against rebuild (add-bvh-refit, #194). A mesh layer's topology
    # is fixed, so a stamp leaves the tree's shape valid and only its bounds
    # stale: the rebuild is proportional to the MESH and the refit to the
    # BRUSH. This is the whole claim of that change, held as a ratio so it is
    # machine-independent — both sides move together on a slower runner.
    #
    # The ceiling is deliberately far above what it measures (~0.0006x here:
    # 0.027 ms against 45.7 ms on ~130k triangles), because the failure this
    # catches is CATEGORICAL: a refit that started walking the mesh — most
    # easily by summarising an ancestor from its span instead of from its two
    # children — lands at ~1x, not at 0.0006x.
    #
    # ONE MESH SIZE, so this gate holds the RATIO and not the slope. The slope
    # is what the change is actually about, and it is held by the pair of sizes
    # below instead.
    ("BM_BvhRefitDab", "BM_BvhRebuild", 0.05),
    # THE SLOPE, which the ratio above cannot show. The same brush-sized dab on
    # a mesh four times the size must cost about the same, because a refit is
    # proportional to the BRUSH. 2.5x leaves room for the deeper tree (one more
    # level of ancestors to walk) and for cache effects on the larger arrays,
    # and still fails loudly at the 4x a mesh-proportional refit would show.
    ("BM_BvhRefitDabBig", "BM_BvhRefitDab", 2.5),
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
    # vertex at a time on one core measured 14x there. The ceiling catches the
    # serial path coming back on capable machines without flaking on small
    # runners, where both sides lose the pool together.
    #
    # RAISED 10.0 -> 14.0 by the blocked tape evaluator (add-cpu-simd-path task
    # 1.4), and the reason matters because NOTHING REGRESSED — both sides got
    # faster and the ratio still grew. One Linux desktop, medians of 7:
    #
    #   main      6.80 ms / 0.993 ms = 6.85x
    #   blocked   5.48 ms / 0.546 ms = 10.0x
    #
    # The denominator gained more (1.84x against 1.25x) because a dab refill is
    # distance-only evaluation, which is exactly what walking the tape once per
    # block helps most, while the gradient pass pays four tetrahedron taps and
    # their buffer traffic per point and so keeps less of it. The gate's premise
    # — that both sides move together — holds against RUNNERS, which is what it
    # was written for, and not against a change that speeds one side more.
    #
    # The failure it exists to catch is not weakened by the new ceiling, it is
    # easier to see: serial taps measured 14x against the OLD denominator, and
    # against the faster one the same fallback lands near 26x. 14.0 sits well
    # above the healthy 10 and well below that.
    ("BM_MeshBricksGradDenseDoc", "BM_DabRefillDenseDoc", 14.0),
    # Undo of a stamp stroke (accel/undo-removal): undoing 100 stamps on a
    # 10k-stamp document is gated against the same stroke on a 100-stamp one.
    # Before the location index in SdfContent every removed stamp's locate()
    # walked the whole node arena, so this ratio scaled with document size
    # (33x between these sizes); after, both sides pay the same per-stamp
    # hash work. 3x catches the O(document) walk coming back, not noise.
    ("BM_UndoStampsGrownDoc", "BM_UndoStampsFreshDoc", 3.0),
    # The coloured add combine against a reference that computes it ONCE (#225).
    # `split-the-combine` let ctape_combine_values obtain the add case's blend
    # weight from a SECOND ctape_smin_m, on the reasoning that CSE would make it
    # free. AppleClang on arm64 does not eliminate it: 1.63x on the combine and
    # 1.23x end-to-end on the device's one scalar coloured tape case. Nothing
    # here saw it — the gated benchmarks are distance-only and got faster — and
    # the device gate passed it at 1.13x against a 1.40 tolerance.
    #
    # A MUCH TIGHTER CEILING THAN ANYTHING ABOVE, and deliberately so. The
    # generosity in the rest of this file buys tolerance for a runner that is ~3x
    # slower, which the two sides of those pairs do not absorb equally because
    # they are different work. These two are the same work: the reference does
    # one ctape_combine_dist, one cmix, the same eight loads and the same four
    # accumulator adds, over the same L1-resident operands in the same process.
    # The healthy ratio is ~1.0 by CONSTRUCTION rather than by measurement, so
    # runner speed cannot move it and nothing but the kernel doing extra work
    # can. Measured on an M-series Mac at these settings: 0.96-0.98x fixed,
    # 1.55-1.56x post-#223, with the reference itself identical in both builds
    # (2.86-2.93 ns/call). 1.25 sits between them with ~25% on either side.
    #
    # On a toolchain that DOES eliminate the second call there is no regression
    # to catch and this reads its healthy value. That is the gate working: it
    # charges for the duplication on the machine that actually pays for it.
    ("BM_TapeCombineAddColored", "BM_TapeCombineAddColoredRef", 1.25),
    # Rebuilding the whole-document tape after an APPEND against compiling it
    # from scratch (#197 phase 1). A sculpt grows a node per brush stamp and a
    # host raycasts to place the next one, so this rebuild is on the
    # interactive path and used to be the whole document every time.
    #
    # A RATIO rather than a ceiling, because both sides do the same work on the
    # same document in the same process and only one of them reuses the
    # compiled prefix. What it catches is the fast path silently ceasing to
    # fire — a refused checkpoint, an invalidation that stopped recording
    # appends, a prefix copied twice — all of which land at or above 1.0x.
    #
    # Measured quiet at 50 000 items: 0.544 ms against 10.3 ms, so 0.053x.
    #
    # THE CEILING IS NOT SET FROM THAT, and the reason is worth recording,
    # because it is the trap a tighter number would have walked into. The two
    # sides do not have the same bottleneck: reuse is a 7.8 MiB memcpy and is
    # memory-bandwidth bound, while the full compile is dominated by per-item
    # work and is compute bound. So a loaded runner does NOT move them
    # together — measured on this machine with two other jobs at ~98% CPU, the
    # append side went 0.544 -> 3.4 ms while the compile side barely moved,
    # and the ratio degraded from 0.053x to 0.31x. A ceiling read off the
    # quiet number would fail on a busy CI runner and teach the next person to
    # distrust the gate.
    #
    # 0.50 sits an order of magnitude above the quiet measurement, above the
    # worst contention observed, and still 2x below the 1.0x that a lost fast
    # path reads.
    ("BM_WholeDocAppend50000", "BM_WholeDocCompile50000", 0.50),
]

# counter gates: (bench, counter, max_value) — the named counter must be at
# most max_value. SKIPPED when the benchmark is absent, the way FASTER_THAN is
# and unlike MAX_RATIO, because a GPU benchmark only registers where a device
# is present and CPU-only CI must not fail for lacking one.
#
# This exists because some properties are not times. Whether an appended tape
# is patched onto the resident one or re-uploaded whole is, on a discrete GPU,
# almost invisible in wall clock: measured on an RTX 5060 the patched stroke
# is 48.7 ms against 57.8 ms, because both pay the same ~48 ms of dispatch and
# fence latency. The same pair is 0.333 ms against 9.44 ms of HOST CPU, and
# reallocates 0 times against 300 — and the reallocation count is exact,
# deterministic and the same on every machine, where a 16% wall-clock margin
# on a GPU is a flaky gate waiting to happen.
#
# The allocator churn is also the half of #197 that matters most on the
# platform the issue is actually about: a desktop with 8 GB of VRAM shrugs at
# re-allocating a 3 MiB buffer per stamp; an iPad is where memory pressure
# ends sessions.
MAX_COUNTER = [
    # A stroke re-packs when it outgrows its reserved slack, which is
    # geometric — measured 8 re-packs over 8 154 appends, and 0 over the 300
    # this benchmark runs. Per-dab reallocation, which is what the code did
    # before patching and what a regression would restore, is 300.
    ("BM_VulkanStrokePatched", "repacks", 4),
    # The same claim on Metal (#296), and on Metal it is the WHOLE gate. The
    # Vulkan pair at least moves the wall clock 1.19x; on unified memory the
    # two rows measure 49.0 ms against 50.2 ms, because both evaluate the same
    # 40k-instruction tape with the same dispatch and differ only in what the
    # host copied first. A time gate on 1.02x would flake on any machine. The
    # reallocation count does not: 0 against 300 over the same stroke, exact
    # and machine-independent.
    ("BM_MetalStrokePatched", "repacks", 4),
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
    counters = {}
    for bench in data.get("benchmarks", []):
        name = bench["name"].split("/")[0]
        seen.add(name)
        times[name] = bench.get("real_time", 0)
        counters[name] = bench
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
    for name, counter, max_value in MAX_COUNTER:
        if name not in counters:
            continue  # no device here; see the note on MAX_COUNTER
        got = counters[name].get(counter)
        if got is None:
            failures.append(f"{name}: counter '{counter}' missing from results")
            continue
        print(f"bench-gate: {name}: {counter}={got:,.0f} (ceiling {max_value:,})")
        if got > max_value:
            failures.append(f"{name}: {counter}={got:,.0f} above ceiling {max_value:,}")
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
