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
  source callback 12; test functions at most 14. Layering check passes.

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

Full CPU suites, focused sanitizers and paired current-main application validation
are in progress. Issue #531 remains open.
