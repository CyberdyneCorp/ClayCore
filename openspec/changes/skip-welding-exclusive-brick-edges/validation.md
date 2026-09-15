# Prototype validation

Three alternating process pairs compare twelve full/subset sphere, box and 48-Grab fixtures (216 timings). Every result matches all 36,019,240 reference bytes for positions, indices, normals, colors and brick ranges, SHA-256 `2a7cfac38a13c1f4dfff484c694c733e373cc1b87237d462d276c27cf26e449c`. The quiet-start guard reports no sustained CPU contention. Earlier contended screening runs are excluded.

Full-sphere medians improve 19.545→13.361 ms without attributes and 22.910→17.329 ms with gradient normals/color. Full-box medians improve 5.909→3.639 and 7.386→4.886 ms; full deformed-sphere medians improve 17.931→10.868 and 24.598→18.526 ms. Subset results are approximately unchanged. These are isolated prototype measurements, not production or application results. Sources and evidence are retained under `/tmp/clay-531-interior-welding/`. Production verification remains pending.

## Production correctness in progress

Three focused cases pass 929,521 assertions, including independent neighboring-box ownership checks, eligibility boundaries and complete mesh/reference comparisons. All dimensions 1–16 are checked for edge ownership; exact mesh comparisons include dimensions 1/2/8/16/32, reversed and repeated requests, empty/subset/full requests, attributes, LOD and nested concurrent execution.

ASan/UBSan with leak detection passes 43 cases and 2,345,543 assertions across meshing, brick caches, sample reuse, edge welding and ownership tests. Clang-tidy cognitive complexity with macro expansions excluded reports eligibility 11, interior classification 6, Builder edge emission 4, mesh replay 30, and new test helpers at most 6. All 70 strict OpenSpec items pass. All eleven CPU CTests pass in 244.06 seconds: 2,777 C++ cases / 17,868,176 assertions, plus 757 Python cases and one intentional skip. Production timing and combined application verification remain pending.

## Production allocation requests

A counter-enabled probe compares the previous and current production meshers on the twelve fixtures; all output hashes match. Full-sphere allocation requests fall from 76,188,544 to 65,220,744 bytes without attributes. Allocation calls increase from 18,296 to 19,337 because eligibility temporarily checks requested-key uniqueness with a hash set. These are cumulative allocation requests during meshing, not peak resident memory; timed results from the allocation-instrumented executable are excluded. Initial zero-counter output came from accidentally using the uninstrumented timing probe and is excluded. The corrected run asserts nonzero allocation counts. Source is `/tmp/clay-531-weld-allocation-probe.cpp`; data and summary are under `/tmp/clay-531-interior-welding/`.
