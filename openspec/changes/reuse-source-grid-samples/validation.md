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

The earlier 32-item prototype probe gave 216.646 → 160.831 ms (25.8%), with exact
output. Production remeasurement and application validation are pending. These
engine probes alone do not establish the 16 ms application target.

## Remaining validation

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
- Paired current-main application timing is pending a quiet CPU window.
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
