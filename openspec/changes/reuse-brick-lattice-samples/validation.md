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
