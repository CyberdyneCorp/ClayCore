# Prototype validation

Three alternating process pairs compare twelve full/subset sphere, box and 48-Grab fixtures (216 timings). Every result matches all 36,019,240 reference bytes for positions, indices, normals, colors and brick ranges, SHA-256 `2a7cfac38a13c1f4dfff484c694c733e373cc1b87237d462d276c27cf26e449c`. The quiet-start guard reports no sustained CPU contention. Earlier contended screening runs are excluded.

Full-sphere medians improve 19.545→13.361 ms without attributes and 22.910→17.329 ms with gradient normals/color. Full-box medians improve 5.909→3.639 and 7.386→4.886 ms; full deformed-sphere medians improve 17.931→10.868 and 24.598→18.526 ms. Subset results are approximately unchanged. These are isolated prototype measurements, not production or application results. Sources and evidence are retained under `/tmp/clay-531-interior-welding/`. Production verification remains pending.

## Production correctness in progress

Three focused cases pass 929,521 assertions, including independent neighboring-box ownership checks, eligibility boundaries and complete mesh/reference comparisons. All dimensions 1–16 are checked for edge ownership; exact mesh comparisons include dimensions 1/2/8/16/32, reversed and repeated requests, empty/subset/full requests, attributes, LOD and nested concurrent execution.

ASan/UBSan with leak detection passes 43 cases and 2,345,543 assertions across meshing, brick caches, sample reuse, edge welding and ownership tests. Clang-tidy cognitive complexity with macro expansions excluded reports eligibility 11, interior classification 6, Builder edge emission 4, mesh replay 30, and new test helpers at most 6. All 70 strict OpenSpec items pass. All eleven CPU CTests pass in 244.06 seconds: 2,777 C++ cases / 17,868,176 assertions, plus 757 Python cases and one intentional skip. The combined application with Core `ebf084ab` and host `259526a` passes all 90 enabled cases (73 library, 17 native/rendered), with one informational case ignored and no adapter skips. Its preserved SHA-256 is `7bc2ce3c8a102c8931c4a7a21288324b2e6afa1d1fe31ef0a5a5689c78ff01f0`. The committed v0.113.0 vendor pin is restored. These correctness runs experienced CPU contention; their incidental timing output is not performance evidence. Production and application timing results follow below.

## Production allocation requests

A counter-enabled probe compares the previous and current production meshers on the twelve fixtures; all output hashes match. Full-sphere allocation requests fall from 76,188,544 to 65,220,744 bytes without attributes. Allocation calls increase from 18,296 to 19,337 because eligibility temporarily checks requested-key uniqueness with a hash set. These are cumulative allocation requests during meshing, not peak resident memory; timed results from the allocation-instrumented executable are excluded. The counter-enabled run asserts nonzero allocation counts. Source is `/tmp/clay-531-weld-allocation-probe.cpp`; data and summary are under `/tmp/clay-531-interior-welding/`.

## Production component timing

The actual production library completes three alternating process pairs against the previous library across the twelve fixtures (216 timings). All six serialized outputs match the full 36,019,240-byte reference exactly. The quiet-start guard reports no sustained CPU contention. Full-sphere medians improve 19.615→13.439 ms without attributes and 22.902→15.352 ms with gradient normals/color. Full deformed-sphere medians improve 19.998→11.350 and 26.491→16.825 ms. Subset medians are approximately unchanged. These are component results; the completed all-brush application comparison follows below. Sources, rows and summary are `/tmp/clay-531-interior-welding/compare-production.py` and `production-{timings,summary}.json`.

## Completed application comparison

Ten alternating pairs complete 260 cases across all thirteen brushes without command timeouts or sustained CPU contention. Every paired begin/continue/end upload count matches.

| Action | Previous median ms | Exclusive-edge median ms |
|---|---:|---:|
| move end | 45.001 | 37.863 |
| smooth begin | 61.482 | 57.727 |
| smooth end | 65.418 | 60.612 |
| relax begin | 68.003 | 66.288 |
| relax end | 66.445 | 61.776 |
| move-topological end | 46.645 | 47.951 |

Other brush results are mixed, including ordinary releases where this meshing shortcut does not remove the remaining layout work. These are fixture medians, not per-run guarantees; empirical p90, maxima and counts above 16 ms are recorded in the summary. The all-brush 16 ms goal remains unmet. Raw rows and summary are `/tmp/clay-531-exclusive-live/measurements.json` and `/tmp/clay-531-exclusive-live-summary.json`. Platform CI remains pending.
