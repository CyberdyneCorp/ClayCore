# Mesh sculpting: improvement priorities

Investigation date: 2026-09-16. Engine baseline: `96fc007f` (`origin/main`).
Scope: engine performance, surface quality and stroke integration. No production
algorithm was changed during this investigation.

## Recommendation

**Start with adaptive remesh latency, followed by a shared adaptive stroke
consumer.** The first has a measured latency problem in this fixture; the second
removes a confirmed integration gap. Treat both as separate OpenSpec changes.

| Priority | Improvement | Evidence and expected value | Acceptance before shipping |
|---|---|---|---|
| 1 — performance | Reduce adaptive remesh work per stamp | Adaptive Draw stamp median 6.77 ms, sampled p95 21.36 ms, maximum 27.97 ms. Topology timer mean 10.59 ms versus total stamp mean 11.52 ms. This is the clearest measured source of frame overruns. | Repeat alternating baseline/candidate runs with identical strokes, topology settings and operation counts. Preserve deterministic output and undo; demonstrate reduced tails without silently lowering detail. |
| 2 — sculpting workflow | Add `brush::apply_to_dynamic` and its public bridge | Shared consumers exist for fixed meshes and multires, while adaptive callers must assemble strokes from stamps. Shared kernels and automasking already exist. | Pressure, spacing, orientation, masks, Grab/Snakehook anchors and one-gesture undo; compare topology-disabled strokes with the fixed path, and exercise split/collapse during anchored strokes. |
| 3 — quality | Evaluate detail-preserving adaptive remesh policy | Local adaptation is shipped; it should be evaluated on long pulls, thin folds, creases and masked boundaries before adding new policy. This investigation does **not** establish a quality defect. | Record surface deviation, volume change on closed fixtures, triangle quality, feature displacement and topology operations. Retain determinism and explicit operation budgets. Add regression fixtures for demonstrated defects. |
| 4 — performance | Optimize shared workset weighting and normal refresh | On fixed Draw, these two stages each account for about one third of recorded stamp time; Draw arithmetic is about 0.1%. These stages also serve other brushes. | Identical positions, normals, masks and dirty chunks; fixed-footprint scaling at increasing model sizes; all affected brush families and deferred-normal strokes. |
| 5 — multires | Measure and reduce crossing-transition setup cost | `MultiresSculptor::stamp_coarse` creates temporary coarse sculptors per stamp; coarse telemetry is not forwarded. This is a code-level hypothesis, not a measured bottleneck. | Compare interior and rim-crossing strokes, allocation counts and first-use cost. Preserve released-cache lifetime rules, watertight export and exact undo. |

Before changing remesh algorithms, instrument candidate collection, split,
collapse, flip/relax and index maintenance separately. Record operation counts
**per stamp** and whether a pass exhausts its budget. An operation limit bounds
work count, but does not guarantee a wall-clock deadline. Do not introduce a
time-dependent topology policy without explicitly resolving its determinism
and output-quality consequences.

## Measurements

Release CPU library, Linux, Intel i9-12900K, affinity
`0,2,4,6,8,10,12,14` (one logical CPU per performance core). The matrix waited
for three one-second samples with CPU idle at least 75% and load below 5.
No build or test ran concurrently with it; the guard observed no sustained
contention. System CPU samples include the waiting period in `cpu-samples.json`.

The fixture is the existing checkerboard-corrugated plane with 0.01 spacing.
Footprint means requested radius-derived count, not guaranteed moved vertices.
Four warm-up stamps precede each cell. Fixed mode uses 20 measured stamps;
other modes use 12. These are exploratory samples, not reliable population
p95/p99 estimates or an application frame-time guarantee.

| Mode / brush | Actual vertices | Requested footprint | Median measured time | Sampled p95 | Maximum |
|---|---:|---:|---:|---:|---:|
| Fixed Draw | 1,000,000 | 1,000 | 0.64 ms | 0.68 ms | 0.71 ms |
| Fixed Draw | 1,000,000 | 10,000 | 4.74 ms | 5.89 ms | 6.12 ms |
| Fixed Smooth | 1,000,000 | 10,000 | 5.80 ms | 6.38 ms | 6.56 ms |
| Fixed Relax | 1,000,000 | 10,000 | 5.49 ms | 6.64 ms | 7.63 ms |
| Adaptive Draw | 99,856 initially | 1,000 | 6.77 ms | 21.36 ms | 27.97 ms |
| Multires Draw, level 2 | 97,969 | 1,000 | 0.61 ms | 0.73 ms | 0.74 ms |
| Layered multires Draw, level 2 | 97,969 | 1,000 | 0.78 ms | 0.93 ms | 0.94 ms |

**Timing boundaries differ:** fixed rows include seed lookup, chunk query, stamp,
index update and CPU payload copy. Adaptive rows measure stamp/remesh only
(copy median 0.066 ms). Multires rows measure stamp only: unlayered detail write
median 0.215 ms and copy 0.116 ms are separate. Layered commit is outside the
stamp loop, so its release cost is not established here. Do not compare these
rows as equivalent end-to-end operations or sum stage percentiles.

Adaptive uses BrushRelative detail and the existing default settings. Its
measured sequence performs 4,724 topology operations in total. The fixture
evolves across stamps, so per-stamp workload can change substantially. Stage
timers can nest (normal refresh inside writeback), so their percentages are not
an exclusive partition of wall time.

At fixed footprint 1,000, Draw including copy measured 0.663 ms on 99,856 vertices
and 0.641 ms on 1M. This supports locality for this fixture; it does not reproduce
the old whole-model scaling problem. Draw's larger footprint ended with 7,507
workset vertices; Smooth/Relax with 10,004. Their timings are not a same-output
brush comparison.

### Readback measurement scope

The original `bench_extreme_poly.cpp` calls `SurfaceView::copy_chunk` with null
buffers: that queries capacities, not payload. The retained diagnostic patch
adds reusable position/normal/index buffers and checks returned status. On the
1M/10k Draw cell, readback median changes from 0.010 ms for capacity queries to
0.604 ms for actual copying. These are successive diagnostic runs, not a
production speedup. The inherited `upload KB` estimate omits normal bytes.
Neither path measures GPU upload, UI/rendering, or input-to-display latency.

The diagnostic also selects Draw/Smooth/Relax and enables existing adaptive
stage telemetry. Production benchmarking should expose these choices and
separate cold activation, continuous strokes, commit, payload copying and
display upload. This is a finding about this benchmark's scope, not a claim
that no other gate exercises payload copying.

## Quality and capability review

- Fixed meshes deliberately preserve indices/quads. Stretching under a large
  pull is not itself a violation; adaptive topology is the density-changing mode.
- Dynamic stamps share brush kernels and support undo. Layer is explicitly
  declined because split vertices have no defined stroke-start reference.
  Defining that reference is a separate design problem.
- Multires, sculpt layers, regional refinement, cross-level neighborhoods and
  watertight mixed-depth export are implemented. Recommending their initial
  implementation would duplicate completed work.
- Existing tests cover topology-off parity, deterministic adaptive strokes,
  masks, undo/redo, work locality, operation budgets under utility load, and
  multires transition behavior. More artist-shaped quality fixtures should
  extend these contracts rather than replace them with visual impressions.
- Regional coarse sculptors intentionally avoid retaining caches across memory
  pressure releases. Reusing them blindly could create invalid references.

The comparison document and Phase 6 roadmap still contained obsolete claims
that adaptive/multires or transition support were absent. This investigation
updates those capability statements.

## Issue review

The open-issue API response contained two actual issues: [#531](https://github.com/CyberdyneCorp/ClayCore/issues/531)
and [#530](https://github.com/CyberdyneCorp/ClayCore/issues/530), both SDF work;
the other entries were PRs. No open mesh-specific issue was found in that snapshot.
The mesh/sculpt issue search returned 39 closed issues.

Relevant historical reports are [#192](https://github.com/CyberdyneCorp/ClayCore/issues/192)
(dab scaling), [#194](https://github.com/CyberdyneCorp/ClayCore/issues/194)
(BVH cost), [#368](https://github.com/CyberdyneCorp/ClayCore/issues/368)
(activation), and [#405](https://github.com/CyberdyneCorp/ClayCore/issues/405)
(coarse forms versus detail). They are context, not reproduced current defects.
This matrix does not isolate cold activation: its printed setup includes fixture,
topology and index construction. Reproduce activation independently before
reopening that concern. No new issue has been filed by this investigation.

## Verification and reproduction

Current-main focused tests passed: **74 cases, 9,105 assertions** across
`test_dynamic_sculpt.cpp`, `test_mesh_sculpt_parity.cpp`,
`test_multires_regional.cpp`, `test_sculpt_kernels.cpp`,
`test_dynamic_stress.cpp`, `test_dynamic_scale.cpp`, and
`test_dynamic_history.cpp`. The two test logs are retained beside this report.
This was not a full-suite or sanitizer run, and no optimization has been
implemented or validated as faster yet.

Documentation checks: `git diff --check` passed; `openspec validate --all
--strict` passed all 74 items.

To reproduce the diagnostic in an isolated checkout of `96fc007f`:

1. Apply `probe.patch` using `git apply --unidiff-zero` and build the release
   `bench_extreme_poly` target with the CPU-only preset.
2. Set `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6` if required by this
   Linux environment. Set `MESH_PROBE_BRUSH=draw`, `smooth`, or `relax`.
3. Set `MESH_PROBE_COPY=1` for payload copies; unset it for capacity queries.
4. Run with the affinity above and `--which=fixed --sizes=100000,1000000
   --footprints=1000,10000 --reps=20 --levels=2` for each fixed brush. Run Draw
   once more with copying unset.
5. Run each of `--which=adaptive`, `multires`, and `layers`, with
   `--sizes=100000 --footprints=1000 --reps=12 --levels=2` and copying enabled.

Raw logs preserve stage means, sample tails and work counts. The patch is
diagnostic evidence, not a proposed production benchmark implementation.
