# Validation — source-grid sample reuse (#531)

## Scope and baseline

Engine baseline: `8ab1659de0e2a26b4f18c887e4a23fea873547f2` (updated main).
Only complete uncomposed source grids use sharing. Partial fills, composed prefix
coverage, sample coordinates, brick storage and file formats retain their existing paths.

## Regression coverage

- Three focused cases / 127 assertions pass. Coordinate reconstruction checks every
  sample in three rectangular grids (6×6×6, 7×8×9, 13×13×13), independently through
  `BrickGrid::sample_position`, for all three coordinate bit patterns.
- One evaluator call and exactly `(8*bx+1)*(8*by+1)*(8*bz+1)` evaluations are required.
- Invalid dimensions, overflows, partial/empty windows, small grids and grids whose
  unique scratch would exceed old scratch decline without writing or evaluating.
- Actual two-item source materialization agrees byte for byte with duplicate-point
  evaluation, including bounds and full-after-partial materialization.
- A temporary mutation retaining eligibility but evaluating duplicate positions fails
  27 assertions in the new regression (45 still pass). This demonstrates the count
  gate detects losing sample reuse; it is not a claim about an old coverage gap.
- Clang-tidy cognitive complexity, IgnoreMacros=true: shape guard 11, fill helper 13,
  source callback 12; test functions at most 14; retained probe main 22. Layering check passes.

## Controlled engine probe

The retained `probes/materialization.cpp` accepts an output blob path and source item
count. It uses real SdfSourceField evaluation and materialization on a 0.02-cell
sphere lattice. Six fixtures cover initial full/local materialization, full/local
after an earlier local fill, and repeated materialization. Build with the selected
revision's release library, meshoptimizer archive, include path and pthread.

Initial production one-item comparison: three alternating process pairs, seven
samples per fixture, first sample discarded, pinned to CPU 8. All complete output
blobs match. The initial full materialization median is 10.685 → 8.964 ms (16.1%).
The other fixtures take fallback paths; their small mixed differences are not
attributed to this optimization.

Production 32-item comparison, using the same alternating-pair protocol:
initial full materialization improves 214.076 → 156.149 ms (27.1%). All complete
output blobs match across all six runs. Partial-fill fixtures retain the existing
path and show small mixed differences, which are not attributed to this change.
The run completed without sustained CPU contention. These engine probes alone
do not establish the 16 ms application target.

## Correctness checks

- All 11 CPU suites pass (108.15 s): 2,787 C++ cases / 17,880,581 assertions and
  757 Python tests, one skipped. This was a correctness run alongside builds,
  not a performance comparison.
- All 93 enabled combined desktop cases pass: 76 library and 17 agent/sculpt/
  settling/rendered cases. Two library tests remain ignored; no adapter skip.
- Strict OpenSpec: all 74 items pass.
- Focused unoptimized ASan/UBSan with leak detection: all 104 cases /
  10,080,171 assertions pass across source-grid, prefix-cache, sculpt, volume and
  relax tests; no sanitizer diagnostics. The unoptimized cached-prefix fixtures
  make this a long correctness check, not a performance measurement.
- Paired current-main application timing completed: 260 cases, all paired upload counts match.
  Issue #531 remains open.

Desktop comparisons use host `4663b68` from PR #137 unchanged in both builds.
Baseline engine `8ab1659d`; candidate engine `7eb59930`.
Preserved executable SHA-256:
- Baseline: `453c6fb129228e5ebfa84ce822986868fe5f64ae9cd87518a48c1a918d86dcc5`
- Candidate: `19ebedd79a4a94fe315c62899c042b77072aba037ed569fb92d94864d6e06903`

For a 13×13×13 grid, evaluated samples fall from 1,601,613 to 1,157,625 (27.7%).
Requested scratch payload falls from 19,219,356 to 18,522,000 bytes (3.6%). The
new path uses two temporary allocations instead of one; these payload calculations
exclude allocator metadata, backend allocations and process RSS.


## Application comparison on performance cores

Ten alternating baseline/candidate pairs cover 13 brushes, with begin, continue
and release measured for each. Every brush starts from the clean sphere by undo.
Each stroke uses begin at `[0,0,1]` with pressure 1, continue at `[0.12,0,1]`
with pressure 1, then release. Each process has a fresh state directory.
Both processes use CPUs `0,2,4,6,8,10,12,14`, one logical CPU per performance core
on the Intel Core i9-12900K. All builds and tests finished before timing.

The successful sweep completed all 260 cases without a command timeout or
sustained CPU contention. Every paired action's upload count matches. CPU idle
from the first brush setup through completion was 85.5% median, 73.9% minimum;
no sample in that interval was below the guard's 40% threshold. The guard required
three idle≥75%, load<5 samples before starting. An earlier six-case attempt was
interrupted for sustained contention and excluded completely from these results.
Affinity and low load do not control clock changes, GPU contention or all noise.

### Median latency (baseline → candidate, milliseconds)

| Brush | Begin | Continue | Release |
|---|---:|---:|---:|
| mask | 6.553 → 6.535 | 0.036 → 0.032 | 8.489 → 8.469 |
| crease | 1.298 → 1.316 | 0.016 → 0.017 | 18.206 → 17.640 |
| clay | 2.135 → 2.590 | 0.017 → 0.027 | 14.676 → 14.706 |
| inflate | 2.082 → 2.083 | 0.030 → 0.022 | 14.859 → 15.428 |
| layer | 1.481 → 1.437 | 0.021 → 0.019 | 13.958 → 13.702 |
| standard | 1.574 → 1.562 | 0.026 → 0.021 | 14.337 → 14.255 |
| polish | 0.035 → 0.031 | 0.015 → 0.016 | 35.758 → 34.054 |
| planar | 0.026 → 0.026 | 0.014 → 0.014 | 35.476 → 34.628 |
| move-topological | 0.028 → 0.024 | 0.014 → 0.013 | 58.174 → 52.647 |
| move | 0.023 → 0.027 | 3.013 → 3.128 | 40.629 → 43.005 |
| relax | 64.188 → 63.820 | 0.047 → 0.060 | 67.596 → 65.306 |
| smooth | 61.189 → 57.669 | 0.033 → 0.029 | 64.877 → 66.271 |
| snake-hook | 0.023 → 0.024 | 14.061 → 14.054 | 25.433 → 25.012 |

Smooth preparation improves 61.189 → 57.669 ms (5.8%); nine of ten paired runs
improve, with a median paired change of −3.515 ms. Relax preparation is nearly
flat in aggregate (64.188 → 63.820 ms); its paired changes are mixed. Other
actions also move in both directions, including paths this optimization does not
change, so this comparison does not establish a general brush-latency gain.
The 16 ms target remains unmet for preparation and several releases.

### Candidate tails (p90 / maximum, milliseconds)

P90 is the ninth sorted observation of ten. Small sample counts make these
observed tails, not a device-wide latency guarantee.

| Brush | Begin | Continue | Release | Actions over 16 ms (begin / continue / release) |
|---|---:|---:|---:|---:|
| mask | 6.845 / 6.950 | 0.059 / 0.064 | 8.812 / 8.859 | 0 / 0 / 0 |
| crease | 1.990 / 2.027 | 0.029 / 0.030 | 20.150 / 21.380 | 0 / 0 / 10 |
| clay | 3.060 / 3.132 | 0.034 / 0.052 | 16.013 / 16.903 | 0 / 0 / 2 |
| inflate | 2.792 / 3.018 | 0.038 / 0.059 | 17.853 / 18.709 | 0 / 0 / 3 |
| layer | 2.195 / 2.328 | 0.047 / 0.060 | 16.761 / 17.740 | 0 / 0 / 3 |
| standard | 1.936 / 2.250 | 0.041 / 0.136 | 14.714 / 15.202 | 0 / 0 / 0 |
| polish | 0.040 / 0.046 | 0.022 / 0.035 | 35.579 / 35.879 | 0 / 0 / 10 |
| planar | 0.044 / 0.094 | 0.037 / 0.047 | 36.838 / 36.916 | 0 / 0 / 10 |
| move-topological | 0.035 / 0.040 | 0.017 / 0.025 | 65.564 / 66.799 | 0 / 0 / 10 |
| move | 0.036 / 0.039 | 3.896 / 4.125 | 45.644 / 51.215 | 0 / 0 / 10 |
| relax | 69.096 / 71.930 | 0.105 / 0.110 | 69.858 / 72.544 | 10 / 0 / 10 |
| smooth | 59.007 / 60.037 | 0.039 / 0.053 | 69.234 / 69.997 | 10 / 0 / 10 |
| snake-hook | 0.032 / 0.036 | 15.808 / 17.249 | 26.734 / 27.064 | 0 / 1 / 10 |

## Reproducibility

Each source-probe run serializes all six fixtures and seven repetitions into
143,271,576 bytes. Every baseline/candidate run has the same SHA-256 per source:
- One item: `eca8045a12d6b86d7db57f2b918562b22c29cb4b10ba0f1835cb78daee5267bf`
- 32 items: `aafe30edecee2c7ad0301365d817a3b6ef7aacea84f103354d3cfb4f02da1c2d`

The private helper retains the existing evaluator, sample coordinates and brick
layout. Review found no new mutable shared state or dependency-layer violations.
Platform CI remains the separate cross-platform gate; local validation here is
Linux CPU plus the enabled desktop renderer checks.
