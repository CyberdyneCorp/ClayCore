# Validation

The row-classification prototype completes three alternating process pairs across twelve full/subset sphere, box and 48-Grab fixtures (216 timings). All six outputs reproduce all 36,019,240 reference bytes, SHA-256 `2a7cfac38a13c1f4dfff484c694c733e373cc1b87237d462d276c27cf26e449c`. Full-sphere no-attribute meshing improves 13.822→12.837 ms and its 48-brick subset 3.914→3.248 ms. Eleven fixture medians improve; the attributed full box is slightly slower. These are prototype component figures, not application latency. Source and results are under `/tmp/clay-531-ring-rows/`.

The production helper uses bounded three-bit row masks with no allocation. An independent closed-box test checks dimensions 1–16 and 32, nonpositive dimensions, empty/full/single-neighbor masks and deterministic asymmetric masks. All 1,140 comparisons pass, including exact order and absence of duplicate cells. All eleven CPU CTest suites pass in 86.75 seconds, including the C++ shards and Python suite. The CTest progress log is `/tmp/clay-531-ring-rows-ctest.log`; no aggregate assertion total is claimed for that run.

ASan/UBSan with leak detection passes 44 selected cases / 2,346,683 assertions across meshing, brick caches, ownership, welding, sample reuse and row enumeration. Clang-tidy with macro expansions excluded reports complexity 23 for local enumeration, 10 for each classification helper, 2 for the call-site wrapper, and at most 10 for test helpers. The unsupported GCC-only warning flag is omitted from the Clang invocation; source include/define/build flags are retained. All 71 strict OpenSpec items pass.

Production timing and allocation results follow below; combined application validation remains pending. The all-brush 16 ms goal remains open.

## Production component comparison

The actual production implementation completes three alternating process pairs across all twelve fixtures (216 timings). Every complete 36,019,240-byte output matches the reference. Eleven fixture medians improve: full-sphere meshing is 13.605→13.224 ms without attributes and 17.375→15.939 ms with normals/color; its uncolored 48-brick subset is 3.881→3.175 ms. The full 48-Grab attributed fixture is slower, 19.214→20.440 ms. These are component medians, not application guarantees. No sustained CPU contention is detected. Rows and summaries are `/tmp/clay-531-ring-rows/production-{timings,summary}.json`.

The counter-enabled production comparison preserves all 36 output hashes. All eighteen uncolored allocation count/byte pairs match, including full sphere at 19,337 allocations and 65,220,744 cumulative requested bytes. Attributed counts vary within and between process runs, so identical attributed allocation totals are not claimed. The row helper itself adds only bounded stack arrays. Instrumented timings are excluded. Source and results are `build-production-probes.py`, `before-memory.csv`, `production-memory.csv` and `memory-summary.json` in the same temporary directory.
