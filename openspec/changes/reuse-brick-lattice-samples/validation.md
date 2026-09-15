# Prototype evidence

A temporary implementation is linked ahead of the current otherwise-identical library. Three alternating process pairs each run twelve full/subset sphere, box and 48-Grab cases with three timed repetitions (216 timings). All six executions serialize identical complete output: 60 vectors / 36,019,240 bytes, SHA-256 `2a7cfac38a13c1f4dfff484c694c733e373cc1b87237d462d276c27cf26e449c`.

Median full sphere mesh cost is 24.197→19.912 ms without attributes and 27.422→22.535 ms with gradient/color. Full box is 8.017→5.884 ms without attributes. The 48-Grab sphere with gradient/color is 29.761→25.055 ms. All twelve case medians improve. The quiet-start guard reports no sustained CPU contention. These are isolated prototype measurements, not application latency or production regression proof.

Scratch sample payload is 17³ × 4 = 19,652 bytes per active worker, in addition to existing local edge recording. Production checks and application measurements remain pending. Local source, binaries, exact outputs and summary are retained in `/tmp/clay-531-sample-reuse-prototype/`.

## Production checks in progress

The production build passes. Focused meshing and brick regressions pass 72 cases / 1,448,790 assertions, including the new sample-block cases and existing exact reference comparisons for subsets, LOD, attributes, dimensions and nested parallel dispatch. All 68 strict OpenSpec items pass. Full CPU and sanitizer validation are running.

Clang-tidy cognitive complexity with assertion-macro expansions excluded is 6 for the sample-block constructor and 8 for its regression helper; the dimension-loop test scores 1. Macro-inclusive scores are also retained in the local report (the helper appears as 68 due to expanded doctest assertions), rather than attributed to handwritten control flow.

The final production library also reproduces all 60 complete reference vectors byte-for-byte: 36,019,240 bytes and the same SHA-256 above. This is separate from the temporary prototype comparison.

The first UBSan run caught signed overflow in coordinate indexing at the new extreme-origin fixture: an intermediate sum used a global y coordinate before subtracting the origin. Both the test callback and production lookup now parenthesize each coordinate difference before scaling/adding local offsets. The extreme-origin regression is retained; sanitizer and final CPU verification must be rerun after this correction.

After the index-order correction, ASan/UBSan with leak detection passes all 72 focused cases / 1,448,790 assertions. The initial CPU suite was intentionally interrupted after its first three shards passed because it used the earlier build; a final CPU rebuild and complete rerun are required. Changed marching helpers (`march_cell`, `march_cells`, `record_brick`) each score 6 with assertion macros excluded.

The corrected CPU library again matches all 36,019,240 reference bytes. The combined application build succeeds with Core `264eed46` and the dense-remapping host code. Its preserved binary has SHA-256 `827a40489b1d33083d50adcd801e91e9f7d17b0076ca70f983f585fee48d29a1`; the application vendor checkout is restored to its committed v0.113.0 pin. Combined correctness and same-base application timing remain pending.

## Final CPU verification

All 11 CTest targets pass in 237.99 seconds on the corrected build: 2,774 C++ cases / 16,908,730 assertions and 757 Python tests with one intentional skip. The earlier interrupted suite is not counted. Together with the corrected 72-case sanitizer run, this covers final index arithmetic rather than the superseded implementation.

All 87 enabled combined application cases pass in one isolated-display run (70 library and 17 native/rendered cases), with one informational timing test ignored and no adapter skips.

## Final production component timing

Three alternating process pairs complete 216 timings against the corrected production library. All six complete serialized outputs match the same 60-vector reference exactly. The CPU guard reports no sustained contention.

| Fixture / keys / attributes | Before ms | Production ms |
|---|---:|---:|
| 0 / 1043 / 0 | 23.712 | 20.047 |
| 0 / 1043 / 1 | 27.885 | 22.925 |
| 0 / 48 / 0 | 4.182 | 3.872 |
| 0 / 48 / 1 | 4.904 | 4.412 |
| 1 / 336 / 0 | 8.345 | 6.282 |
| 1 / 336 / 1 | 9.416 | 8.032 |
| 1 / 48 / 0 | 3.456 | 3.131 |
| 1 / 48 / 1 | 3.675 | 3.460 |
| 2 / 1044 / 0 | 24.647 | 19.078 |
| 2 / 1044 / 1 | 29.029 | 25.000 |
| 2 / 48 / 0 | 4.131 | 3.884 |
| 2 / 48 / 1 | 4.763 | 5.041 |

Fixture IDs 0/1/2 denote sphere, box and 48-Grab sphere; attributes 1 means gradient/color. Most medians improve, but the 48-key Grab attribute case increases slightly. These component results do not prove an across-brush application gain. The same-base live comparison is running with diagnostics enabled for command timeouts.

## Same-base application timing

Ten alternating pairs complete 260 cases across 13 brushes, with identical host code and only the engine sample-reuse increment differing. All paired uploaded byte counts match for begin, continue and end. The quiet-start guard reports no sustained contention, and no command timeout occurs. This successful run does not establish the cause of the earlier remapping-run undo timeout.

| Brush | Begin before / fixed ms | Continue before / fixed ms | Release before / fixed ms |
|---|---:|---:|---:|
| mask | 5.868 / 6.162 | 0.033 / 0.031 | 7.521 / 7.841 |
| crease | 1.060 / 0.920 | 0.014 / 0.014 | 22.935 / 23.525 |
| clay | 2.377 / 2.026 | 0.018 / 0.018 | 17.410 / 17.125 |
| inflate | 2.116 / 1.852 | 0.017 / 0.015 | 17.232 / 16.735 |
| layer | 1.165 / 0.972 | 0.016 / 0.016 | 16.550 / 16.350 |
| standard | 1.159 / 0.966 | 0.016 / 0.016 | 16.583 / 16.558 |
| polish | 0.033 / 0.020 | 0.016 / 0.012 | 35.243 / 34.272 |
| planar | 0.023 / 0.024 | 0.012 / 0.017 | 35.200 / 37.073 |
| move-topological | 0.024 / 0.019 | 0.012 / 0.013 | 48.834 / 48.872 |
| move | 0.023 / 0.019 | 3.304 / 2.867 | 51.858 / 44.328 |
| relax | 74.126 / 68.979 | 0.025 / 0.027 | 71.561 / 67.727 |
| smooth | 69.764 / 63.928 | 0.025 / 0.024 | 70.885 / 67.232 |
| snake-hook | 0.027 / 0.025 | 14.893 / 13.311 | 28.910 / 28.000 |

These are fixture medians, not per-run guarantees. Smooth/Relax preparation and Move release improve; other release results are mixed, including a higher Planar median. The 16 ms goal remains unmet. Raw measurements and the summary are retained locally under `/tmp/clay-531-brick-samples-live*`. Platform CI remains pending.
