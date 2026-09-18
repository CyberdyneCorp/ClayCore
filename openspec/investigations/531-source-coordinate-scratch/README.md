# Issue #531: source-coordinate scratch investigation

## Decision

**Do not merge the tested scratch changes.** Three candidates preserve exact
engine output but regress the one-item engine fixtures. The first complete
application comparison also fails to establish an overall latency improvement.
Production code remains at `e7a1938a5a793bb65273b4b9f4798665ec42bf7a` (v0.120.0).
The 16 ms target remains unmet. These records preserve rejected experiments,
not a completed fix or a proposed production implementation.

## Candidates and engine results

All changes are retained as patches against the engine baseline for reproduction.

| Candidate | Patch | Full fill, main → candidate | Full after local, main → candidate |
|---|---|---:|---:|
| CPU native grid evaluation | `implicit-grid-only.patch` | 6.72 → 7.94 ms | 8.23 → 11.68 ms |
| Native grid + partial point workspace reuse | `implicit-grid-with-reuse.patch` | 6.72 → 7.82 ms | 8.07 → 11.59 ms |
| Existing point evaluator + full coordinate workspace reuse | `coordinate-reuse.patch` | 6.54 → 8.06 ms | 8.06 → 11.59 ms |

The corresponding `engine-*.json` files contain all six fixtures for one- and
32-item sources. All paired output hashes match byte for byte, including volume
blobs, bounds, tallies and added-brick coordinates. The 32-item full-fill results
are approximately unchanged, not evidence of a substantial improvement.

Probe: `../../changes/reuse-source-grid-samples/probes/materialization.cpp`.
Build separately with each revision's release CPU library and meshoptimizer
archive, C++20, `-O3 -DNDEBUG`, pthread. Run three alternating process pairs,
seven samples per fixture, discard the first, affinity CPU 8. Output argument is
a binary blob path; final argument is source item count (1 or 32). Compare full
blob hashes before interpreting timings. Summary values are pooled medians of
18 retained samples per fixture/version.

## Complete application comparison: native grid candidate

Desktop `eeb158a`, built in an isolated worktree against current main and the
first candidate, with otherwise identical build settings. Auto-detection selects
CUDA. Source filling explicitly uses CPU in both versions; this is not a
CPU-only desktop run and must not be compared as a trend to older CPU-only data.

Ten alternating process pairs × 13 brushes = 260 cases. Fresh application state
per process, clean sphere per brush (undo between brushes), begin `[0,0,1]`,
continue `[0.12,0,1]`, pressure 1, then end. Affinity `0,2,4,6,8,10,12,14`.
All corresponding phase upload byte counts match, and all phases report CUDA.

| Phase | Main median | Native-grid median |
|---|---:|---:|
| Smooth begin | 48.69 ms | 46.29 ms |
| Smooth end | 57.54 ms | 59.93 ms |
| Relax begin | 51.12 ms | 50.19 ms |
| Relax end | 55.90 ms | 58.46 ms |
| Move Topological end | 41.19 ms | 41.73 ms |
| Move end | 34.24 ms | 34.07 ms |
| Snakehook end | 22.96 ms | 24.23 ms |

See `application-native-grid.json` and the phase measurements for all brushes.
The second candidate's application run was stopped when its component regression
persisted; that incomplete run is excluded from the comparison.

Timing waits for three quiet CPU samples (at least 75% idle, load below 5).
A guard aborts after four consecutive 250 ms samples below 40% idle during
measurement. Startup is untimed and followed by another quiet-window wait.
No local builds/tests overlap timing. This limits contention; it does not
eliminate per-core, GPU, frequency or scheduling noise. An earlier initial
baseline startup attempt was excluded before any measurements were collected.

## Correctness evidence for the first candidate

- Warm allocation gate: main 18,522,232 bytes; candidate 4,630,748 bytes (~75%
  fewer requested bytes), threshold 9,261,000. Test compiled against saved main
  fails and against candidate passes. This measures allocation requests, not RSS.
- Release CPU unit suite: 2,838 cases / 17,930,932 assertions passed.
- Focused ASan/UBSan with leak detection: 110 cases / 10,084,258 assertions passed.
- Host library 76 passed / 2 ignored; sculpt latency 4 passed; settlement 5
  passed; visual sculpting 18 passed; agent end-to-end 2 passed / 1 failed.
- The agent export-consent assertion at `agent_end_to_end.rs:687` fails
  identically against the saved main application. The entire host suite is not
  green; this failure is reproduced on baseline.
- Cognitive complexity: shape guard 11 unchanged; reconstruction 13 → 7;
  native source evaluator 8; coordinate-test helper 6. These scores describe the
  rejected native-grid candidate, not a retained production change.

Later candidates received paired exact-output checks; do not attribute the
first candidate's full-suite/sanitizer results to them. They were rejected
before final verification. Their patches may include tests not yet run.

## Diagnostic findings and next direction

Running the fallback fixture alone removes much of its apparent before/after
difference. Explicit allocator mmap thresholds change the relative timings too.
This suggests allocation history affects results; it does not prove a unique
cause. Hardware profiling was unavailable (`perf_event_paranoid=4`); system
security settings were not changed.

The small pointer-down improvement does not approach 16 ms, and several releases
still exceed the target substantially. The next investigation should measure
current host phases separately: Smooth/Relax transaction priming, preview-sample
transfer, mesh extraction, duplicate pruning, layout and upload. Current host
code primes the whole surface and rebuilds geometry when entering the live
preview. Establish which work can be reused before designing a partial preview.
Any reuse must preserve the complete surface, exact lattice coordinates, normals,
undo/cancel behavior and interaction with other visible layers; deferring the
same blocking work to release would not satisfy #531.

## Current host phase attribution

Three additional instrumented baseline processes, same 13-brush protocol and
CPU guard. `host-phase-probe.patch` applies to desktop `eeb158a`; measurements are
in `host-phases.json`. These are diagnostic timings, separate from the unmodified
paired comparison. Temporary host instrumentation was removed afterward.

| Phase (median, ms) | Smooth begin | Smooth end | Relax begin | Relax end |
|---|---:|---:|---:|---:|
| Prime source materialization | 7.41 | — | 8.04 | — |
| Absorb preview samples | 5.06 | — | 5.48 | — |
| Engine mesh extraction | 12.94 | 19.35 | 14.49 | 19.66 |
| Read mesh | 1.59 | 1.46 | 1.85 | 1.47 |
| Split mesh by brick | 5.30 | 4.05 | 4.98 | 4.22 |
| Total remesh | 20.19 | 25.29 | 21.69 | 25.22 |
| Pruning, layout and upload | 8.90 | 8.63 | 9.33 | 8.73 |

`upload` in the raw log includes duplicate pruning and CPU layout work inside
`SurfaceGeometry::upload`; it is not a GPU transfer measurement. The remesh total
includes its three component rows, so do not add those rows again. Independent
medians need not add exactly. These spans do not account for the entire event.

The preview requests 1,023 keys, and the release rebuild requests 1,043. The next
high-value optimization is reducing or reusing whole-surface mesh work while
preserving exact preview and settled surfaces. The engine mesh itself already
exceeds 16 ms on release, so improving just allocation or upload cannot finish
#531. Neither the issue nor its 16 ms goal is complete.

## Repository checks

After restoring production code: OpenSpec strict validation **81 passed, 0
failed**; layering and diff whitespace checks pass. No production optimization
from these experiments is retained.
