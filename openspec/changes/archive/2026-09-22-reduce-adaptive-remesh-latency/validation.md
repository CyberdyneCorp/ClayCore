# Adaptive remesh normal batching: validation

## Change and invariant

Remeshing invokes private topology operators which refresh normals immediately
for the first 64 successful updates, then collect affected face and vertex
handles. After all topology passes, the collector sorts and deduplicates complete
handles (slot **and generation**), skips dead handles, refreshes each surviving
normal once and synchronizes the gesture delta. It flushes before relaxation.

Topology eligibility checks calculate face normals from geometry; they do not
read the stored normals being postponed. Public split/collapse/flip calls retain
immediate refresh. Operation ordering, thresholds, constraints and budgets are
unchanged. The collector adds temporary memory proportional to affected handles
across the operation sequence; it does not allocate a whole-model slot array.

## Correctness

- Release: 110 cases / 47,927 assertions passed, covering dynamic surfaces,
  spatial queries, sculpting, remesh convergence/constraints/budgets, topology
  operators, history, stress and allocation gates.
- ASan + UBSan + leak detection: 94 cases / 47,401 assertions passed.
- New regression: four curved/open fixtures, 300 interleaved operations each;
  exact positions, normals, colors, masks and exported connectivity versus
  immediate refresh; operation refusals agree; recycled slots are asserted;
  encoded undo and redo restore exact expected surfaces; repeated flush is safe.
- Layering check passed. No public ABI or file-format change.

Test filters: `*/test_dynamic*.cpp,*/test_remesh_local.cpp,*/test_topology*.cpp`,
plus `*/test_sculpt_allocation.cpp` for the release run. These are targeted runs,
not claims of full-suite or GPU/application validation.

## Complexity

Measured using the cognitive-complexity skill and clang-tidy with actual include
paths, C++20 and doctest macros excluded from the score. The skill's default
invocation omits these include paths and yields incomplete parsing here; the
retained explicit invocations are the meaningful measurements.

New routines: update dispatcher 2, collector update 4, flush 14, regression 13.
Existing split 38, collapse 39 and remesh driver 132 are unchanged versus source
read from main revision `96fc007f`. These remain above the systems target and
are existing refactoring debt; this change does not claim to resolve them.
The operator write sequences remain together for review of their atomic rewiring.

## Performance reproduction

Baseline mesh code: `96fc007f`. The later main commits through `aafeccb6` touch
SDF source initialization and documentation, not these mesh paths.

Apply `evidence/comparison.patch` with `git apply --unidiff-zero` in isolated
baseline and candidate checkouts, then build the release CPU-only
`bench_extreme_poly` target. The diagnostic adds brush selection, actual payload
copying, adaptive stage telemetry, optional gesture recording, operation-count
output, and final exported geometry/normal/color/connectivity output.

Run both binaries on the same host with:

```sh
MESH_PROBE_COPY=1 MESH_PROBE_UNDO=1 MESH_PROBE_OUTPUT=result.bin \
  taskset -c 0,2,4,6,8,10,12,14 ./bench_extreme_poly \
  --which=adaptive --sizes=100000 --footprints=1000 --reps=20
```

Unset `MESH_PROBE_UNDO` for unrecorded stamps; repeat at footprint 10000. This
Linux environment also uses `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6`.
Three process pairs alternate baseline/candidate ordering for each configuration.
Four warm-ups precede twenty measured stamps, all within one evolving fixture;
when enabled, the gesture record also spans the warm-ups. Payload copying and
final export are outside the stamp/remesh timer. No GPU upload or pointer-down
activation measurement is included.

The host is an Intel i9-12900K, using one logical CPU per performance core.
Wait for three one-second samples with CPU idle at least 75% and load below 5;
stop on sustained contention. Do not run builds or tests concurrently.

## Paired release results

Each value below is the **median of three per-process statistics**, with twenty
measured Draw stamps per process. In particular, p95 is a small-sample statistic,
not a reliable population tail estimate. All times are stamp/remesh milliseconds.

| Requested footprint | Undo | Median before → after | Sampled p95 before → after | Median run maximum before → after |
|---|---|---|---|---|
| 1,000 | Off | 5.907 → 5.934 | 18.405 → 15.549 | 23.270 → 18.974 |
| 1,000 | On | 6.054 → 5.926 | 20.283 → 17.429 | 25.882 → 23.107 |
| 10,000 | Off | 7.880 → 7.551 | 63.554 → 46.014 | 66.071 → 48.360 |
| 10,000 | On | 8.067 → 8.087 | 74.363 → 57.310 | 75.422 → 57.845 |

With undo, sampled p95 decreases by 14.1% and 22.9% respectively. It improves in
every paired process run, while medians are mixed and broadly flat. At the small
footprint, mean stamp cost (median of runs) is 8.809 → 8.343 ms with undo; at the
large footprint it is 23.009 → 19.112 ms. The slowest individual candidate stamps
still exceed 16 ms: 24.899 ms at the small footprint and 71.785 ms at the large
footprint with undo. No universal frame-time target is claimed.

All 240 paired measured-stamp operation/refusal records match, including split,
collapse, flip, relax and budget status. The final exported position, normal,
color and connectivity payloads match in all twelve process pairs, with or
without undo. SHA-256 by footprint:

- 1,000: `14287993274ac1208d0145f96d2a94e96e7bc9066ed3db6d5726661d57187795`
- 10,000: `37edfea245e07df4cfc32fcc087eff4e94e8b21f307d3f119cff567686d71f22`

The guard observed no sustained CPU contention during the matrix. CPU samples
include the preceding waiting period. Raw per-run logs, hashes and statistics
are retained under `evidence/`. These compare complete Draw stamp sequences on
one corrugated-plane fixture; they do not establish all-brush, cold-activation
or application performance.

The initial unconditional batching experiment reduced slower samples but
increased the light-stamp median with undo. The retained implementation's
immediate-refresh prefix addresses that overhead; those earlier timings are
not included in the results above.

## Next work

This completes the first measured optimization in priority 1. Adaptive latency
still needs work, especially at large footprints. Next in the agreed feature
order is the shared adaptive stroke consumer, as a separate OpenSpec change;
quality policy changes should continue to preserve explicit detail decisions.
