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
    # Subdividing is the one multires operation a host WAITS on rather than
    # drags through, so the ceiling is generous by design; what it catches is an
    # accidental O(level^2) in the stencil walk, not noise. Measured at 4.3 ms
    # on a 32x32 cage.
    "BM_MultiresSubdivide": {"max_ms": 5_000},
    "BM_MultiresEvalCold": {"max_ms": 5_000},
    "BM_SurfaceNets": {"max_ms": 20_000},
    "BM_MeshLatticeMarch": {"max_ms": 20_000},
    "BM_MeshLatticeNets": {"max_ms": 20_000},
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
    # A Move drag through the C ABI. It exists because NO cpu benchmark covered
    # this path, and that is how `layer_move_surface` lost 1.34x when region
    # invalidation landed and kept it for four releases (#358): the only gate
    # that saw it was the device suite, which cannot run in CI, and its own
    # noise floor then suppressed the failure.
    #
    # THIS CEILING DOES NOT CATCH THAT, and says so rather than implying a gate
    # it cannot be — the same admission BM_VoxelAddLevelWhole above makes. 1.25x
    # is inside the spread between a developer machine and a shared runner, so a
    # threshold tight enough to fail on it would flake instead. Measured 0.077 /
    # 0.840 ms on an M2 Max; these sit ~7x above, for the order-of-magnitude
    # case this file exists for.
    #
    # What it DOES buy is a one-command local A/B for anyone touching
    # apply_edit, which is what the fix was developed against, and a row in CI
    # output where there was silence.
    "BM_MoveDrag1000": {"max_ms": 0.6},
    "BM_MoveDrag10000": {"max_ms": 6.0},
    # The same drag under a layer mirror (#363). Same ceiling as the
    # unmirrored row, because the mirrored drag now warps the same items: the
    # reflected ball touches none of abi_sculpt's copies. A floor rather than
    # only the MAX_COUNTER row below because a floor FAILS when its bench is
    # missing, and the counter gate skips an absent bench by design -- this is
    # what makes the counter gate's presence enforced.
    "BM_MoveDragMirrored1000": {"max_ms": 0.6},
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
    # The meshing spec's "preview is cheaper than marching" claim, measured on
    # ONE precomputed lattice so that the comparison is the MESHERS.
    #
    # It used to be ("BM_SurfaceNets", "BM_MeshTape"), and that pair could not
    # say it. Two things were wrong with it. Until #302 both sides meshed with
    # the default attributes, and the attribute pass was 80-96% of each, so the
    # pair passed on nets emitting 3.2x fewer VERTICES while the geometry step
    # went unmeasured -- and once #302 batched that pass, nets was measured
    # 1.68x SLOWER to build (#304). And even with attributes off, both sides
    # spend about half their time in `eval_tape_grid` on the same field, which
    # compresses whatever the meshers differ by.
    #
    # #304's answer was the mesher rather than the spec: the dual walk was
    # sampling 12 lattice corners per cell through a std::function where it
    # needed 8 inlined, so nets went 113 ms -> 75 ms serial and the claim came
    # back. This pair holds it where it can be seen -- 86.8 ms against 120 ms,
    # no field evaluation on either side.
    ("BM_MeshLatticeNets", "BM_MeshLatticeMarch"),
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
    # And the other half of that dab's cost: the cull index was rebuilt from
    # scratch every stamp, walking every node to recompute bounds that had not
    # moved, so that one appended item could be added. 2.42 ms at 50,000 items
    # against 0.13 ms to extend, and the margin widens with the document rather
    # than being a percentage. Both sides produce the index a rebuild gives --
    # test_cull_index.cpp holds the per-brick tapes byte-identical either way.
    ("BM_CullIndexAppend", "BM_CullIndexRebuild"),
    # And the whole dab, through the ABI a host drives: a refill that RESUMES
    # from its own previous float32 output against one that replays the
    # surviving edit list over every sample. 7.13 ms to 0.13 ms at 20,000
    # items, and the two are bit-identical by contract. Catches the resumable
    # path falling back silently, which is how it is designed to fail.
    ("BM_BrickRefillResumed", "BM_BrickRefillFull"),
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
    # A multires dab costs what it TOUCHED, not what the hierarchy holds
    # (add-mesh-multires). Both sides reconstruct the same surface at the same
    # display level: the cold one rebuilds every level from the cage, the local
    # one propagates one dab's descendants. A hierarchy whose dab cost tracked
    # its size rather than the brush's reach would be correct and unusable, and
    # this pair is what makes that visible rather than asserted -- measured at
    # 0.87 ms against 16.3 ms on a 24x24 cage at three levels (Linux desktop,
    # loaded), and the margin widens with depth rather than narrowing.
    ("BM_MultiresDabLocal", "BM_MultiresEvalCold"),
    # And a dab at the FINEST level propagates to nothing at all, so it is the
    # cheapest of the three by construction. The pair holds that: a detail pass
    # that started costing a coarse pass would mean detail had stopped being
    # stored where it is written.
    ("BM_MultiresDabFine", "BM_MultiresDabLocal"),
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
    # THE CHAIN PAD, MEASURED WHERE IT BITES (#335). #282 pads a per-brick cull
    # region by the largest single-item reach in the layer, which is 4k for a
    # quadratic profile; the region it pads is a fixed brick plus band, so the
    # survivor count grows superlinearly in k. Every cull row here blended at
    # k = 0.03, where the pad cost the "20-35%" that change recorded, and the
    # same pad measured 1.87x on a document blending at 0.06 — an ordinary
    # sculpt, and the frame-path regression ClaySpaceDesktop reported at 1.76x
    # against a fixture set that could not show it.
    #
    # The same document and the same eight bricks at twice the blend radius, so
    # the runner cancels and what is left is the pad. 0.298 ms against 0.248 ms
    # here (1.20x), with the `instrs` counter — the deterministic half — at
    # 4,590 against 2,802. The ceiling is well above that because the failure
    # worth catching is the pad widening again, not a few per cent of drift.
    ("BM_DeepDocCullPlanned2000K06", "BM_DeepDocCullPlanned2000", 2.0),
    # EXTENDING THE CULL INDEX MUST COST THE DAB, NOT THE DOCUMENT (#347). The
    # same one-item append onto a document ten times the size must measure the
    # same, because what an append does is bounded by the subtree it adds.
    #
    # It did not. `CullIndex::append` re-walked the touched layer's whole flat
    # node map to recompute the cull pad -- a walk the change that introduced it
    # defended as cheap, and it was: 0.15 ms against the 2.45 ms rebuild it
    # replaced. Against a resumed dab it was not. Measured on a quiet twelve-core
    # Linux box at 20,000 and 2,000 nodes: 0.0542 ms against 0.00454 ms, an
    # 11.9x slope, which is the document showing through -- and this gate reads
    # 12.5x on the unfixed code.
    #
    # Raising the pad from the appended subtree instead leaves 0.000258 ms
    # against 0.000257 ms -- 1.00x quiet, 1.03-1.13x over three loaded runs, with
    # one 1.8x seen while another job had the box. A RATIO, so a slow runner
    # moves both sides together; 4.0 sits well above the contended reading and
    # well below the 12x a document-proportional append reads. Both benchmarks pin their
    # iteration count and rebuild their fixture, because each append grows the
    # document it appends to and the number of appends must not be a property of
    # the machine.
    ("BM_CullIndexAppend", "BM_CullIndexAppendSmall", 4.0),
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
    # DIRTY-PREFIX (FRONTIER) TRACKING (#360): one frame of a continuing Move
    # drag — drag plus window refill — with the pre-drag prefix seeds kept,
    # against the same frame with the resume budget at zero. Warm, every
    # dirtied brick folds only the dragged suffix onto its prefix; cold, every
    # frame replays the 2000-item chain per brick. Measured 0.12-0.16x on a
    # loaded 24-core Linux box. The failure this catches is categorical — a
    # frontier path that stopped firing lands at ~1x (the fixture's own guard
    # SkipWithErrors first when the warm row stops resuming) — so 0.5 is the
    # usual generosity, not a tight delta.
    ("BM_MoveDragRefill", "BM_MoveDragRefillCold", 0.50),
    # The same warm frame under a LAYER MIRROR (#363), against its own cold
    # row: the frontier path must save the same fraction with a mirror on as
    # without one. Not the detector for the selection defect -- on this
    # fixture the base sits outside even the mirror-expanded bound, so the row
    # resumed before the brush was reflected; BM_MoveDragMirrored1000's
    # warped_ratio below is the row a revert fails. The fixture guard
    # SkipWithErrors when a warm frame stops resuming, and a missing MAX_RATIO
    # name is a FAIL.
    # Its own cold row rather than the unmirrored one because a mirrored
    # document carries twice the geometry on both sides of the ratio (every
    # bound and every brick tape pay for the copy): 0.63 ms against 8.6 ms,
    # 0.07x, where the unmirrored pair reads 0.30 against 1.97, 0.16x -- the
    # full walk pays for the copies per brick and the resumed suffix does
    # not. The same 0.50 generosity; a lost fast path lands at ~1x.
    ("BM_MoveDragRefillMirrored", "BM_MoveDragRefillMirroredCold", 0.50),
    # The spec's own <=5% overhead line, held directly: MARKING a live
    # 288-entry store (touch_region_from, one dirty_from write per kept entry)
    # must not cost more than the legacy DROP of the same store under the same
    # bound (touch_region, which frees every entry's vectors). Measured
    # 0.11-0.13x — marking is cheaper than dropping, not merely within 5% of
    # it — so the spec's 1.05 ceiling carries ~8x headroom and still reads as
    # the spec's number.
    ("BM_TouchRegionFromSeedStore", "BM_TouchRegionSeedStore", 1.05),
    # Tracking cost must not scale with history length: the same edit over the
    # same 288-entry store under a 5000-root sculpt against a 2-root one. The
    # per-brick frontier state is three numbers whatever the history, and the
    # edit's only O(history) step is one root-list walk for its ordinal.
    # Measured 1.2-1.3x; 3.0 catches an O(history x entries) path coming
    # back, not runner noise.
    ("BM_TouchRegionFromDeepHistory", "BM_TouchRegionFromSeedStore", 3.0),
    # -- live SDF sculpt transactions (sdf-sculpt-transaction) ---------------
    #
    # SMOOTH, before and after. The old path had nowhere to keep a baked volume
    # between pointer events, so a dab was bake-the-layer, relax, discard.
    #
    # THIS COMMENT DESCRIBED A DIFFERENT BENCHMARK UNTIL sdf-prefix-cache
    # LANDED, and the correction is recorded rather than quietly swapped in.
    # What it used to say was that a transaction "bakes once at pointer-down"
    # and quoted 0.152 ms against 29.9 ms, or 0.0051. `begin` no longer bakes at
    # all -- it is lazy, evaluates nothing, and a dab materializes only the
    # bricks its own relax will read -- so the whole-layer cost was not moved
    # again but broken up, and this row now carries a share of it.
    #
    # RE-MEASURED on the same 24-thread Linux desktop: 0.307 ms against 28.7 ms,
    # or 0.0107. THE GATE IS UNCHANGED AT 0.10 and it passes by a NARROWER
    # margin than it used to -- about 9x of headroom where the old reading had
    # 20x -- which is stated plainly rather than smoothed over by loosening the
    # number. Nothing got slower: the row is a fixed ~28 ms of first-dab
    # materialization plus ~0.15 ms per dab (measured at 20, 200 and 2000
    # iterations: 1.49, 0.31, 0.162 ms), the 0.15 being exactly what this row
    # read when `begin` baked, and the ~28 ms being that bake arriving at the
    # first dab because on a 193-node unit sphere one 0.25-radius dab covers
    # most of the band. See the note on the benchmark itself.
    #
    # Generous on purpose, and the failure it catches is categorical rather
    # than gradual: a dab that started sampling the WHOLE layer again (a
    # transaction silently falling back to bake-per-dab, which is the shape of
    # the bug) is not 2x, it is ~1x. A ratio is also the only form this can take
    # -- both sides move together on a slower runner, which a millisecond
    # ceiling on the dab would not.
    ("BM_SdfSmoothTransactionUpdate", "BM_SdfSmoothStandalone", 0.10),
    # And the WHOLE gesture, which is the number that settles the trade: a
    # thousand-dab Smooth through a transaction must cost at most twenty-five
    # standalone dabs, where the old path cost a thousand of them. RE-MEASURED
    # after the lazy begin at 6.26 (179.6 ms against 28.7 ms), where the same
    # row read 6.09 when `begin` was a bake -- the trade barely moved, because a
    # thousand dabs amortise a whole-layer cost whether it is paid at
    # pointer-down or at the first dab. The ceiling of 25 keeps four times the
    # headroom and is still forty times below the 1000x the old path would
    # report if the layer were being sampled per dab.
    ("BM_SdfSmoothTransaction1000", "BM_SdfSmoothStandalone", 25.0),
    # MOVE, before and after, on a 1,032-item layer. `move_brush` prepares from
    # scratch every call, so a live drag re-walks the edit list per pointer
    # event to rediscover which items a FIXED anchor and radius reach;
    # `SdfMoveTransaction` walks it once and a frame is one
    # `resolve_prepared_move` per affected item. 0.00121 ms against 0.0587 ms,
    # or 0.021, against a ceiling of 0.20. With the warp carrying a chain of
    # grabs (reflect-the-brush-not-the-bound) the frame resolves into a warp
    # it keeps, so the row still pays no allocation per item for the resolve.
    ("BM_SdfMoveTransactionUpdate", "BM_SdfMoveResolve", 0.20),
    # The same claim over a whole drag: a thousand frames of prepared drag
    # against one frame of the old path. 27.0 measured (1.59 ms against
    # 0.0587 ms), so the traversal pays for itself inside twenty-eight frames --
    # under half a second of dragging. 100 is the ceiling, which a per-frame
    # traversal coming back would blow by an order of magnitude.
    ("BM_SdfMoveTransaction1000", "BM_SdfMoveResolve", 100.0),
    # THE SCALING CLAIM, and the sharpest of these gates. Both benchmarks are
    # PARAMETERISED over the same four unrelated-item counts with the affected
    # set held at 32, and the loop above keys on name.split("/")[0], so the rows
    # that land here are the last registered -- Args({50000}) on both sides,
    # 50,032 items, which is exactly where a per-frame traversal is most
    # visible. 2.87 ms against 0.00121 ms there, or 0.00042; the ceiling is 0.05,
    # a hundred and twenty times above it, and the same gate at the smallest row
    # would read 0.14 because move_brush over 132 items is nearly free. The
    # per-frame row is FLAT across the four sizes -- 0.001208, 0.001244,
    # 0.001246, 0.001213 ms -- while move_brush climbs 336x over the same span.
    ("BM_SdfMoveTransactionUpdateScaling", "BM_SdfMoveResolveScaling", 0.05),
    # -- the history a dab stops paying for (sdf-prefix-cache) ---------------
    #
    # #306's workload, held as the ratio it is about: a FIXED 12 dirty bricks
    # evaluated through `SdfSourceField` against a growing edit list, once with
    # a built prefix and once with none. Both sides evaluate the identical
    # points through the identical entry point and are bit-identical where the
    # volume covers the window (test_sdf_prefix_cache.cpp holds that half); they
    # differ only in whether the old roots were sampled into a volume first.
    #
    # A SCALING LAW RATHER THAN A PERCENTAGE, which is why there are two sizes
    # and not one. The suffix is 64 roots whatever the document holds, so the
    # margin widens with the history exactly as the seeded-dab pair above does.
    # Measured on a 24-thread Linux desktop, two runs at load average 1.1-4.1:
    #
    #   spread  5,000    2.27 / 80.8 ms = 0.028      (0.030 on the second run)
    #   spread 20,000    2.24 / 270  ms = 0.0083     (0.0082)
    #   piled   5,000    2.28 / 86.5 ms = 0.026      (0.030)
    #   piled  20,000    2.25 / 257  ms = 0.0088     (0.0085)
    #
    # THE TWO SIDES DO NOT SHARE A BOTTLENECK, and the ceilings are set from
    # that rather than from the measurement. The full walk goes through the CPU
    # backend's pool and the seeded path is serial, so a runner with fewer cores
    # makes the FULL side slower and the ratio BETTER -- the safe direction --
    # while a machine with more cores than this one moves it the other way.
    # 0.25 and 0.10 sit eight to twelve times above what is measured on
    # twenty-four threads and still an order of magnitude below the ~1.0 a cache
    # that stopped firing would read.
    ("BM_SdfHistoryPrefixSpread5000", "BM_SdfHistoryFullSpread5000", 0.25),
    ("BM_SdfHistoryPrefixSpread20000", "BM_SdfHistoryFullSpread20000", 0.10),
    ("BM_SdfHistoryPrefixPiled5000", "BM_SdfHistoryFullPiled5000", 0.25),
    ("BM_SdfHistoryPrefixPiled20000", "BM_SdfHistoryFullPiled20000", 0.10),
    # THE BUILD, WHICH MUST NOT BE HIDDEN BEHIND THE FOUR HITS ABOVE. Building a
    # prefix is a whole-layer bake of the roots behind the boundary and it is
    # not free: 467 ms at 5,000 roots and 1,876 ms at 20,000 on this desktop,
    # for 268 stored bricks and 1.43 MiB either way -- the same shell, sampled
    # against four times the history. The GATE is that linearity, held as a
    # ratio so the runner cancels: 4.0x measured over 4x the roots, against a
    # ceiling of 10.0. A build that went quadratic in the history reads 16x.
    #
    # Generous rather than tight for a second reason: the 20,000 row runs ONE
    # iteration by design, because a whole-layer bake left to fill a time budget
    # is minutes of CI for a number that gets no more accurate, and one iteration
    # on a shared runner is the noisiest form a measurement takes.
    ("BM_SdfPrefixBuildSpread20000", "BM_SdfPrefixBuildSpread5000", 10.0),
    ("BM_SdfPrefixBuildPiled20000", "BM_SdfPrefixBuildPiled5000", 10.0),
    # POINTER-DOWN NO LONGER COSTS A BAKE. `begin` used to sample the whole
    # finite layer; it now compiles, indexes a lattice and takes a digest, and
    # this holds the claim in the only form that survives a shared runner:
    # beginning a gesture on a 20,000-root layer must cost less than the
    # whole-layer BAKE of a 193-node one. Measured 7.04 ms against 28.7 ms,
    # 0.245 (0.276 on the second run), against a ceiling of 1.0.
    #
    # Categorical, not gradual: a begin that started sampling again would pay a
    # bake of a hundred times the model, which lands near 100x rather than near
    # 1.2x. The `samples` counter below is the exact form of the same check.
    ("BM_SdfSmoothTransactionBegin20000", "BM_SdfSmoothStandalone", 1.0),
    # AND THE SLOPE, which the ratio above cannot show. `begin` is NOT constant
    # and this file does not claim it is -- a compile and a digest are both
    # linear in the root count. What went away is the sampling, which was linear
    # in roots TIMES samples. Four times the roots measured 3.76x and 4.42x over
    # two runs, so the ceiling of 8.0 catches a begin that went superlinear
    # while leaving room for the compile's own constants and for a one-iteration
    # runner. Both sides are the same work on the same machine, so they move
    # together.
    ("BM_SdfSmoothTransactionBegin20000", "BM_SdfSmoothTransactionBegin5000", 8.0),
    # THE FIRST DAB, with a prefix and without. This is where the whole-layer
    # cost went when `begin` stopped paying it, and it is the row a host feels
    # as the pause after pointer-down. 166 ms against 319 ms at 5,000 roots, or
    # 0.52 on a box at load 3.7.
    #
    # THE MARGIN NARROWED ON PURPOSE, and the ceiling was NOT loosened to suit
    # it. An earlier measurement read 0.27, when materialization filled one
    # brick per pooled dispatch; filling consecutive runs instead cut the
    # no-prefix side from 846 ms to 319 and the with-prefix side from 249 to
    # 166, so the prefix's share of the win shrank as the shared cost came out
    # of both. 0.75 still catches the prefix ceasing to help.
    #
    # AND THE COUNTERS SAY WHY IT IS NOT 0.03 like the history rows: a dab's
    # dependency region is a BALL and a prefix volume is a SHELL, so 17 of the
    # windows a first update materializes fall outside the stored band and take
    # the prefix TAPE. The fallback count is gated exactly below, which is the
    # sharper half of this
    # pair; 0.75 here is the loose half, sitting 2.7x above the measurement and
    # still below the ~1.0 a prefix that stopped attaching would read.
    ("BM_SdfSmoothLazyFirstUpdateWithPrefix", "BM_SdfSmoothLazyFirstUpdateNoPrefix", 0.75),
    # THE PREVIEW TRANSPORT, through the C ABI a host draws from.
    # `clay_sdf_smooth_preview_item` copies the whole working volume;
    # `clay_sdf_smooth_preview_delta_take` hands over only the bricks whose
    # bytes are new. 0.0011 ms against 0.0088 ms, or 0.125, with the dab itself
    # paused out of both -- left inside it, the identical update dominated both
    # rows and the pair read 0.154 against 0.151 ms, the same number twice.
    #
    # THE TIME IS THE WEAKER HALF HERE and the ceiling is set accordingly: both
    # sides are memory traffic, so they move together on a slower runner, but
    # they are microseconds wide and a pause/resume sits inside each. The BYTES
    # are the headline and `delta_frac` below gates them exactly. 0.50 catches a
    # delta that started copying the model, which reads 1.0.
    ("BM_CAbiSmoothPreviewDelta", "BM_CAbiSmoothPreviewFullSnapshot", 0.50),
    # -- global voxel remesh (add-voxel-remesher) ---------------------------
    #
    # THE SPARSITY GATE. Same voxel size, one sphere twice the radius of the
    # other: the surface grows 4x and the bounding box 8x. The sampling domain
    # is marked from the source's triangles and their band, so the expensive
    # per-sample work — a BVH distance query carrying a generalized winding
    # number — has to follow the surface. Measured band samples are 815,751
    # against 3,230,930, a ratio of 3.96, which is 4 to two decimal places
    # because that is what the geometry says; the times follow at 4.4-4.6x on
    # a 24-core Linux desktop.
    #
    # The ceiling is 6.0: comfortably above the 4x this is, comfortably below
    # the 8x a revert to `FieldVolume::sample_parallel` over the whole region
    # would land at. That revert is the failure this exists to catch, and it is
    # a CATEGORICAL one rather than a few per cent — which is what makes a
    # ratio gate the right shape here, since both sides move together on a
    # loaded runner.
    ("BM_VoxelRemeshLargeBall", "BM_VoxelRemeshSmallBall", 6.0),
    # SOURCE TRIANGLES SHOULD BARELY MATTER. The same sphere at the same voxel
    # size, tessellated sixteen times as finely: 960 triangles against 16,128.
    # The field is sampled at the same points and the extraction marches the
    # same lattice; only the BVH is four levels deeper, which is logarithmic.
    # 1.27x measured (334 ms against 422 ms on a 24-core Linux desktop). The
    # ceiling of 3.0 catches a per-triangle pass creeping into a per-sample
    # operation — the shape of
    # mistake that makes a remesh usable on a blockout and unusable on the
    # scan it was imported from.
    ("BM_VoxelRemeshDenseSource", "BM_VoxelRemeshCoarseSource", 3.0),
    # THE PREFLIGHT MUST NOT SAMPLE. A host calls the estimate on every tick of
    # a resolution slider, so it walks the triangles and marks the brick
    # lattice and does nothing else — no field, no marched surface. 2.3 ms
    # against the 256 remesh's seconds, or under 0.001. The ceiling is 0.05,
    # twenty-five times above it, and an estimate that started sampling
    # anything would land near 1.0.
    ("BM_VoxelRemeshEstimate256", "BM_VoxelRemeshSphere256", 0.05),
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
    # A brick refill resumes PER BRICK (#342). The gate used to admit a batch
    # only when every brick in it carried a seed at one shared revision, and
    # nothing re-stamps a seed but the refill that writes it -- so a dirty
    # window that MOVED, which is every stroke, mixed the ground the last dab
    # covered with ground it had not and sent all of them down the full walk.
    #
    # The wall clock is the wrong gate for it. Both paths are bit-identical by
    # contract and the fixture is small, so the margin is a property of how much
    # history the benchmark happens to hold. The SHARE OF BRICKS WALKED IN FULL
    # is not: it is a ratio of counts and it is the thing that actually broke.
    #
    # The benchmark primes every window position first, which is what makes the
    # ratio machine-independent rather than merely count-based. Left unprimed it
    # measures a warmup transient amortised over the iteration count, so a slow
    # runner reads higher for no reason -- 0.053 locally against 0.085 on a CI
    # runner, both correct, against a ceiling of 0.10. Primed, every brick asked
    # for has a seed, so resuming per brick this is 0 and a batch-wide gate makes
    # it 1: the whole window, every dab.
    ("BM_BrickRefillMoving5000", "refilled_frac", 0.05),
    ("BM_BrickRefillMoving20000", "refilled_frac", 0.05),
    # The still-window fixture (#348), whose whole point is that the TIMED
    # REGION is the resumed path and nothing else. It primes every brick before
    # the loop, so a correct fixture walks none of them in full and this is 0;
    # a fixture that stopped resuming would report the full path's time under
    # the resumed path's name, which no timing gate here could tell from a
    # slower runner.
    #
    # This benchmark is PARAMETERISED and the loop below keys on
    # name.split("/")[0], so the row that lands here is the last one registered
    # -- Args({48, 20000, 16}), the deepest document at the widest window, which
    # is the row most likely to lose a seed to the store's 64 MB bound and so
    # the right one to hold. Measured 0.000 at all nine rows.
    ("BM_BrickRefillWindow", "refilled_frac", 0.05),
    # -- the prepared drag's per-frame work, in NODES rather than milliseconds -
    #
    # The scaling ratio above says the prepared update is fast at 50,032 items.
    # This says WHY, and says it in a number that is identical on every machine:
    # a frame visits the AFFECTED items and nothing else. The fixture holds that
    # set at 32 and grows only the unrelated bulk of the layer, so `visited`
    # must read 32 at every row -- and `prepare_stats().visited`, reported
    # beside it, reads the whole layer, which is the traversal that was moved to
    # pointer-down rather than removed.
    #
    # PARAMETERISED, so this gates the last row registered -- Args({50000}) --
    # which is the one where a live drag still traversing the tree would read
    # 50,032 instead of 32. The ceiling is 40 rather than 32 so that a fixture
    # tweak of a dab or two is not a CI failure; the failure worth catching is
    # three orders of magnitude away.
    ("BM_SdfMoveTransactionUpdateScaling", "visited", 40),
    # The same guard on the un-parameterised 1,032-item row, so the property is
    # asserted on the row the before/after ratio is quoted from as well.
    ("BM_SdfMoveTransactionUpdate", "visited", 40),
    # What the complexity policy is FOR, held as the number it bounds. A hundred
    # separate drags leave a hundred grabs on every item they all reached --
    # `moved_chain` replaces a leading grab only within one drag -- and each one
    # raises the layer's declared Lipschitz, so `safe_step_scale` fell from
    # 0.848 at ten drags to 0.191 at a hundred with the policy off.
    #
    # With `min_safe_step_scale = 0.5` and consolidation authorised the same
    # hundred drags end at a chain of 4 and 0.540, over 7 consolidations. That
    # is the claim: the chain is HELD, not merely dented.
    #
    # The ceiling is 20 rather than 4 because the exact number depends on how
    # many drags it takes to cross the threshold again after each bake, which
    # is a property of the fixture's geometry. What 20 catches is the failure
    # this gate exists for, and it is a real one that shipped in review: keyed
    # on `consolidation_state` alone, the early-out in `settle_budget` fired the
    # policy ONCE -- consolidating makes that predicate true forever after --
    # and the chain regrew to 58 unattended. A regression there reads 58 or 100,
    # both far above this line, and the policy-off row beside it reads 100.
    ("BM_SdfMoveRepeatedPolicy100", "chain", 20),
    # -- a lazy begin, in the number a wall clock cannot fake ----------------
    #
    # `begin` evaluates NOTHING, so the working volume it hands back stores no
    # sample at all: `samples` is the count read off `preview_volume()`
    # immediately after begin, and a correct row reports 0 where the whole-layer
    # bake this replaced reported 123,930. Exact, deterministic, and identical
    # on every machine -- where a millisecond ceiling on a begin cannot tell a
    # bake from a slow runner. A ceiling of 0 is exact because the check is
    # strictly-greater.
    ("BM_SdfSmoothTransactionBegin", "samples", 0),
    ("BM_SdfSmoothTransactionBegin20000", "samples", 0),
    # -- how the twelve dirty bricks were actually answered ------------------
    #
    # The far-bound rule (see include/clay/session/sdf_prefix_cache.h) sends a
    # window to the prefix TAPE wherever the volume does not store every sample
    # of it. Correct either way, and only the cost differs -- so a cache whose
    # fallback rate is high is a cache that is NOT WORKING rather than one that
    # is wrong, and no wall clock distinguishes that from a busy runner.
    #
    # Measured 0 of 12 at both root counts and both distributions, with the
    # policy's band set wide enough to cover a query brick. The ceiling of 2
    # leaves room for a fixture tweak at the window's edge; the failure worth
    # catching is 12.
    ("BM_SdfHistoryPrefixSpread5000", "fallback_windows", 2),
    ("BM_SdfHistoryPrefixSpread20000", "fallback_windows", 2),
    ("BM_SdfHistoryPrefixPiled5000", "fallback_windows", 2),
    ("BM_SdfHistoryPrefixPiled20000", "fallback_windows", 2),
    # The same question asked of a real dab, where the answer is a SPLIT rather
    # than a clean sweep: a dependency ball reaches inside and outside a stored
    # shell, so some of what a first update materializes takes the tape. That
    # split is geometry and is the same on every machine; it is also the honest
    # explanation of why the pair's time ratio is 0.52 rather than the 0.01 the
    # history rows show. Measured 17 windows -- the count is in WINDOWS, and a
    # window is a run of consecutive bricks that agree, so it moves when the
    # fill's batching does. 60 catches the band being lost entirely.
    ("BM_SdfSmoothLazyFirstUpdateWithPrefix", "fallback_windows", 60),
    # -- the preview transport, in BYTES ------------------------------------
    #
    # THE HEADLINE OF THAT PAIR, and the half that is exact. `delta_frac` is the
    # bytes one frame of the delta path copied divided by the bytes the
    # whole-volume snapshot copies -- 43,463 against 431,568, or 0.1007 -- and
    # it is deterministic: the same dab over the same lattice moves the same
    # bricks on every machine, where the memcpy behind it is a property of the
    # runner's memory bandwidth.
    #
    # 0.25 is 2.5x above the measurement and leaves room for a fixture whose dab
    # path materializes a little differently; a delta that degenerated to the
    # whole volume -- the failure this exists for -- reads 1.0.
    ("BM_CAbiSmoothPreviewDelta", "delta_frac", 0.25),
    # And the build's memory, beside the build's time above: the prefix is a
    # SHELL and its size is a property of the surface rather than of the
    # history, which is why 5,000 and 20,000 roots store the same 268 bricks.
    # A build that stopped culling to the band would store the box.
    ("BM_SdfPrefixBuildSpread20000", "stored_bricks", 600),
    ("BM_SdfPrefixBuildPiled20000", "stored_bricks", 600),
    # A mirrored drag warps what the ball or its reflection touches, in ITEMS,
    # over what the unmirrored drag warps (#363). On abi_sculpt the reflected
    # ball touches nothing, so this is exactly 1.0; selecting on the
    # mirror-expanded bound took 46 items against 22 on the issue's ridge
    # fixture (every item whose expanded bound spans the plane), 2.1x. 2.0 is
    # the issue's acceptance line -- a mirror can at most
    # double what a drag reaches -- and the counter is exact on every machine.
    ("BM_MoveDragMirrored1000", "warped_ratio", 2.0),
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
        # Counts print as integers and fractions as fractions: a ceiling below 1
        # is a ratio, and rounding it to "0" would make a failure unreadable.
        fmt = ",.3f" if max_value < 1 else ",.0f"
        shown = f"{got:{fmt}}"
        print(f"bench-gate: {name}: {counter}={shown} (ceiling {max_value:{fmt}})")
        if got > max_value:
            failures.append(f"{name}: {counter}={shown} above ceiling {max_value:{fmt}}")
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
