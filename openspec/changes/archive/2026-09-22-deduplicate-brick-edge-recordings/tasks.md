## 1. Implementation and reference

- [x] Implement bounded local edge deduplication with general fallback.
- [x] Preserve an independent unoptimized recorder path for regression comparison without expanding the public API.
- [x] Add exact-output regressions covering geometry, attributes, ranges, boundaries, dimensions, LOD and thread counts.

## 2. Verification

- [x] Measure uninstrumented before/after timing and scratch limits across representative workloads.
- [x] Run relevant correctness and sanitizer suites; measure cognitive complexity.
- [x] Run strict OpenSpec validation and update README/roadmap/PR with final evidence.
- [x] Verify platform CI and application behavior; retain #531 as open if its full latency target remains unmet.
