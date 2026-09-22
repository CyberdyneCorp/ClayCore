# Validation — boundary-cell sample reuse (#531)

## Baseline and implementation

Production baseline: `e7a1938a5a793bb65273b4b9f4798665ec42bf7a` (v0.120.0).
The branch also carries the earlier rejected source-scratch investigation; those
patches are evidence only and are not applied to production code.

Only boundary-cell recording changes sampling. A range owns a bounded lazy cache
for one closed brick lattice, dimensions 1–16. It clears validity when the owner
changes, preserving exact integer sampling coordinates. Partial requests, absent
owners, straddler attribution, recording order and global welding retain their
existing behavior. Other dimensions and the reference recorder stay uncached.
There is no API/ABI or serialized-format change and no persistent sample cache.

## Correctness

- Final release C++ suite: **2,839 cases / 17,934,249 assertions**, all passed.
- Final ASan/UBSan suite with leak detection: **46 cases / 2,348,865 assertions**,
  all passed. Covers meshing, brick caches, sample caches, edge ownership/welding.
- Focused regressions: **3 cases / 72,738 assertions**. Tests exact sample bits,
  signed zero/NaN payloads, negative coordinates, high faces, repeated corners,
  owner changes and independent recording ranges. Existing mesh/reference tests
  compare positions, indices, normals, colors, UVs and brick ranges across
  dimensions 1/2/8/16/32, reversed/repeated/full/empty/subset requests, LOD,
  attributes, and nested concurrent execution.
- Disabling validity hits in a temporary helper copy makes both new tests fail:
  **46 assertions fail**. This verifies the tests detect losing sample reuse.
- Current desktop integration: **106 passed / 2 ignored**: library 76, agent
  end-to-end 3, sculpt latency 4, settlement 5, visual sculpting 18. Agent tests
  drive the default-feature candidate binary; test targets were built with the
  required `agent-e2e` feature. No adapter skips. Incidental test timings are not
  benchmark results.
- Clang-tidy C++20 with real includes, macros ignored: new recording helper **5**,
  cache select **3**, lookup **1**, floor division **1**; test traversal **10**,
  retained probe driver **12** (helpers at most **4**).
  Existing straddler collection drops **30 → 27**. It remains a complex geometry
  traversal: its deterministic waves and serial ownership attribution are kept
  together rather than changed as part of this sampling optimization.

## Paired engine timing

Retained source: `probes/mesh.cpp`. Compile against each revision's release CPU
archive and meshoptimizer, C++20, `-O3 -DNDEBUG`, `-pthread`, include paths
`include` and `src/mesh`. Pass an output-blob path to each binary. The retained probe extracts its
  fixture loop into a helper for readability; its output hash was rechecked.

Ten alternating process pairs, twelve fixtures per process, one warm-up excluded
and three recorded samples per fixture: **720 timings**, median of 30 samples per
fixture/version. Affinity `0,2,4,6,8,10,12,14`, libstdc++ preloaded consistently.
All 20 complete output blobs match the 36,019,240-byte reference, SHA-256:
`2a7cfac38a13c1f4dfff484c694c733e373cc1b87237d462d276c27cf26e449c`.

| Fixture | Main median | Candidate median |
|---|---:|---:|
| Sphere, full, no attributes | 13.413 ms | 12.593 ms |
| Sphere, full, gradient normals/color | 16.829 ms | 16.287 ms |
| Box, full, no attributes | 3.699 ms | 3.391 ms |
| Box, full, gradient normals/color | 4.993 ms | 4.808 ms |
| 48-Grab sphere, full, no attributes | 12.232 ms | 11.603 ms |
| 48-Grab sphere, full, gradient normals/color | 19.129 ms | 18.513 ms |

All twelve fixture medians improve, approximately **3–8%**; subset results and
raw rows are in `engine-summary.json` and `engine-timings.jsonl`. The preliminary
three-pair screening run is superseded by this final-code comparison.

The guard waits for three quiet CPU samples (≥75% idle, load <5), and aborts on
four consecutive 250 ms samples below 40% idle. No local builds, test suites or profilers
overlap benchmarking. This reduces contention, without eliminating scheduler
or frequency noise. Engine timing alone does not establish application latency.

## Application timing

Desktop `eeb158a`, default-feature release builds against baseline and candidate
engine, both reporting CUDA. Source mesh extraction is the CPU component above;
this desktop comparison is not a CPU-only run. Ten alternating process pairs,
13 brushes per process, fresh process state, undo to the initial sphere between
brushes. Begin `[0,0,1]`, continue `[0.12,0,1]`, pressure 1, then end. Same CPU
affinity as the engine probe. Startup is untimed, followed by a quiet-window wait.

All **260 cases** complete; all paired begin/continue/end upload counts match.
No sustained CPU contention was reported. `application-summary.json` includes
medians, empirical p90, maxima and counts above 16 ms; phase rows are retained
in `application-measurements.jsonl`.

| Action | Main median | Candidate median |
|---|---:|---:|
| Relax begin | 53.233 ms | 47.142 ms |
| Relax end | 57.057 ms | 56.232 ms |
| Smooth begin | 49.001 ms | 48.260 ms |
| Smooth end | 57.209 ms | 56.002 ms |
| Move end | 33.864 ms | 33.744 ms |
| Move Topological end | 41.704 ms | 41.988 ms |
| Snakehook end | 22.642 ms | 23.080 ms |

These are observed fixture medians, not universal improvements or a per-frame
guarantee. Application results are mixed outside Smooth/Relax; variation and
outliers remain. The all-brush 16 ms goal remains open; this PR does not close
#531. Whole-surface rebuilding and layout remain larger costs.

## Review checks

Strict OpenSpec, module layering and diff whitespace checks pass. Platform CI
is pending on the PR. No public API or geometry format changed.
