# Collapse neighborhood reuse: validation

## Scope and invariant

Baseline: main `8a9d04e3` (v0.118.0), including normal batching (#616) and
recorded-gesture replay (#617). Each collapse now keeps the two endpoint fans
already walked by `one_ring`. Duplicate-face checks, boundary/constraint checks,
geometric checks and pre-write delta capture reuse that unchanged adjacency.
After connectivity changes, outgoing-handle repair and normal refresh still use
fresh topology. Refusal order and floating-point calculations are unchanged.

No public API, ABI, serialization, remesh budget or detail-policy change.

## Regression and correctness

The new allocation regression runs a successful public collapse on 20x20 and
80x80 planar grids. The same test objects linked against the saved main library
fail at both sizes: 126 allocations exceeds the 96-allocation gate. The candidate
uses 80 allocations at both sizes (36.5% fewer); the gate allows modest standard
library variation. Allocation counts describe this fixture, not every collapse.

Release: **136 cases / 65,818 assertions passed**, covering topology operators,
constraints, dynamic surfaces/BVH/sculpting, remesh, history/replay, C dynamic
API/undo and allocation gates. The filter was:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
  build/cpu-only/tests/clay_unit_tests \
  --source-file='*/test_dynamic*.cpp,*/test_remesh_local.cpp,*/test_topology*.cpp,*/test_sculpt_allocation.cpp,*/test_c_dynamic*.cpp'
```

ASan + UBSan + leak detection: **120 cases / 65,287 assertions passed**.
The focused sanitizer binary includes all dynamic/topology tests, remesh, the
C dynamic topology/delta tests and the version test (which supplies test main).
It excludes the global-allocation override used by the release allocation gates.
Compile flags: `-g -std=c++20 -fsanitize=address,undefined
-fno-omit-frame-pointer -fno-sanitize=vptr -fno-sanitize-recover=undefined`.
Leak detection was enabled with `ASAN_OPTIONS=detect_leaks=1`.
These are focused checks, not a claim of full-suite or application/GPU testing.

The first paired run was performed under contention for correctness only. All
12 baseline/candidate pairs have byte-identical exported positions, normals,
colors and indices; all 240 measured stamps have identical operation/refusal
counts and budget flags. Logs and hashes are in `evidence/contended/`. Their
timings are noisy and are **not** used as speedup evidence.

## Complexity and review

clang-tidy's cognitive-complexity check was run on the candidate and source read
from main, with C++20, actual include paths and doctest macros ignored. Changed
helpers: face extraction 3; duplicate-face check 26→24; pinning 4→5; target 7→7;
geometric check 33→31; planning 15→15. The collapse write sequence remains 39
on both revisions, above the systems target; its atomic rewiring sequence is
retained together for review. This change does not resolve that refactoring debt.

Review covered fan lifetime, invalid-input/refusal precedence, boundary and
crease handling, delta capture order, and refresh after mutation. Existing
constraint, topology-stress and replay tests exercise those paths. No unresolved
correctness findings. Layering check and strict change validation passed.

## Reproduction

Apply `evidence/comparison.patch` with `git apply --unidiff-zero` to `benchmarks/bench_extreme_poly.cpp` in
isolated baseline and candidate checkouts, then build the release CPU-only
`bench_extreme_poly` target. The diagnostic uses `stamp_recorded` for undo-enabled
runs, performs actual reusable payload copies outside the stamp timer, prints
operation/refusal counts and exports positions/normals/colors/connectivity.

```sh
MESH_PROBE_COPY=1 MESH_PROBE_UNDO=1 MESH_PROBE_OUTPUT=result.bin \
  taskset -c 0,2,4,6,8,10,12,14 ./bench_extreme_poly \
  --which=adaptive --sizes=100000 --footprints=1000 --reps=20
```

Repeat at footprint 10000; unset `MESH_PROBE_UNDO` for unrecorded stamps. Each
process runs four warmups and twenty measured stamps on an evolving surface.
Use three alternating baseline/candidate process pairs per configuration. The
record spans the warmup and measured stamps. It does not time gesture commit,
undo/replay, cold pointer-down, GPU upload or rendering.

`evidence/profile.patch` (also applied with `--unidiff-zero`) instruments the baseline collapse implementation only.
Its six totals are planning (including refusals), pre-write capture, rewiring,
outgoing repair, normals and final delta synchronization, in microseconds.
Instrumentation overhead means this is a cost breakdown, not speedup evidence.

## Quiet paired results

Linux, Intel Core i9-12900K, release CPU-only build, P-core affinity
`0,2,4,6,8,10,12,14`. All local builds, tests and analysis had finished. The
runner required three consecutive one-second samples with CPU idle at least
75% and one-minute load below 5. During the run it would stop after four
consecutive 250 ms samples below 40% idle. It completed without that stop.
The wait and run samples are retained in `evidence/quiet/cpu.json`.

The table reports **medians of three process summaries**, each with 20 measured
stamps; p95 is a small-sample statistic, not a population latency guarantee.
All values are milliseconds; baseline → candidate:

| Footprint | Recorded | Median stamp | Sample p95 | Mean stamp |
| --- | --- | --- | --- | --- |
| 1,000 | No | 5.807 → 5.701 | 14.905 → 14.016 | 7.579 → 7.342 |
| 1,000 | Yes | 5.885 → 5.714 | 16.987 → 16.056 | 8.103 → 7.866 |
| 10,000 | No | 7.226 → 7.389 | 43.033 → 39.481 | 15.749 → 14.770 |
| 10,000 | Yes | 7.434 → 7.567 | 54.555 → 51.425 | 18.120 → 17.387 |

Recorded sample p95 improves by 5.5% and 5.7%; mean cost improves by 2.9% and
4.0%. The larger-footprint median increases by 1.8%, so this is a modest
improvement in topology-heavy stamps, not an across-the-board speedup. All three
candidate recorded p95 values were below all three corresponding baseline
values at each footprint. Worst observed recorded stamps across the runs:
21.857 → 20.588 ms (1k), and 56.250 → 53.688 ms (10k).

The quiet run independently reproduces all 12 matching exported-mesh hashes
and all 240 matching per-stamp operation/refusal/budget records. Across quiet
and contended runs that is 24 matching pairs and 480 matching stamp records.
The flat, evolving 100k fixture is deliberately bounded: these results do not
establish performance on arbitrary meshes or all brushes, and they do not
establish a consistent 16 ms application frame budget.

The baseline profile on the recorded 10k fixture attributes approximately 31%
of instrumented collapse time to planning, 30% to pre-write capture and 21% to
outgoing-handle repair (including warmups). These are useful next investigation
sites; the absolute profile timings include instrumentation overhead. Retain this
candidate for its deterministic allocation reduction, exact output, passing
correctness checks and repeatable sample-tail improvement. Broader geometry and
application latency remain follow-up work.

## Integration with #619 and Windows allocation gate

Updated local main to `a60d8c16` (v0.119.0) and merged it into this branch without
conflicts. It adds the shared adaptive stroke consumer and its C/Python APIs.
The timing table above remains the comparison of `8a9d04e3` against `18bbfd15`;
it is not a new measurement of the stroke consumer introduced by #619.

The [initial Windows job](https://github.com/CyberdyneCorp/ClayCore/actions/runs/35203273075/job/105142872808)
compiled successfully, then failed only the two allocation-budget assertions:
MSVC STL used **112 allocations** at both fixture sizes, exceeding the Linux
limit of 96. The test now selects a budget of **128 for MSVC STL**, retaining
**96 for the other tested libraries**. It detects the standard library, not the
operating system, and reports the selected limit on failure. The measured
candidate counts are 112 with MSVC STL and 80 with libstdc++.

A separate [Windows main-baseline check](https://github.com/CyberdyneCorp/ClayCore/actions/runs/35206341100)
builds unoptimized `a60d8c16` with the revised allocation test and requires both
fixtures to exceed 128. This checks that the platform adjustment still rejects
the repeated-traversal implementation. The probe workflow is confined to a
scratch branch; it is not added to this PR's regular CI.

After integration, the focused release run passed **154 cases / 69,035
assertions**, including the new dynamic stroke tests. All **80 OpenSpec items**
and the layering check passed. The test's loop/branch structure is unchanged;
clang-tidy parses the updated test successfully. Windows and other platform
results for the final revision are reported by the PR's CI checks.
