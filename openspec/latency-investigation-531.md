# Issue #531: latency measurement and remaining work

## Scope

The 16 ms target across all brush actions remains unmet. This investigation uses ClayCore `dc034eed` (same production code as `01731a00`) and desktop `4663b68` on Linux, with an Intel Core i9-12900K. These are local application measurements, not a guarantee for another device.

## Measurement noise

A 260-case comparison (13 brushes, ten alternating pairs) tested a temporary desktop GPU-buffer reuse implementation. All paired upload counts matched. Builds and tests finished before timing. Aggregate CPU idle was 92.8% at the median and never below 87.9%; no sustained CPU contention or command timeout occurred.

Low aggregate load does not isolate a thread from scheduling delays, GPU contention or clock changes. This processor also mixes performance and efficiency cores. Repeating all 260 cases with both executables restricted to CPUs `0,2,4,6,8,10,12,14` (one logical CPU per performance core) produced matching upload counts, 93.7% median CPU idle and 85.6% minimum idle. The repeat still showed mixed changes:

| Action | Unrestricted control / candidate ms | Performance cores control / candidate ms |
|---|---:|---:|
| Move Topological release | 44.486 / 48.914 | 44.796 / 42.808 |
| Smooth preparation | 53.514 / 52.603 | 54.068 / 54.177 |
| Smooth release | 55.110 / 55.493 | 56.028 / 55.372 |
| Relax preparation | 59.256 / 63.340 | 58.720 / 59.591 |
| Relax release | 54.423 / 54.241 | 55.590 / 55.951 |

These are medians. The direction change for Move Topological demonstrates uncertainty; it does not prove heterogeneous-core migration caused the first result. CPU affinity does not control every relevant variable. Neither a single slow run nor a small median difference establishes a code regression.

The buffer experiment is deferred and is **not in the PR**. Its pinned and combined correctness checks passed, including an identical rendered image after buffer reuse, but no consistent application latency gain was established. A separate actual reservation probe measured roughly 0.001 ms CPU-side allocation versus 0.000032 ms reuse. That probe excludes deferred GPU initialization, submission, rendering and memory residency.

## Remaining Smooth/Relax preparation costs

Temporary diagnostic timers covered preview initialization and full rebuilds, which the earlier stroke-phase report did not include. Eighteen fresh application runs covered six brushes three times each, restricted to the same performance cores and guarded against sustained CPU contention. Timers were removed from production after preserving the diagnostic executable.

| Phase | Relax median ms | Smooth median ms |
|---|---:|---:|
| Prime: engine transaction update | 17.903 | 18.292 |
| Prime: absorb preview into cache | 6.075 | 5.897 |
| Prime total | 23.666 | 24.189 |
| Rebuild: engine meshing | 13.133 | 13.178 |
| Rebuild: mesh readback and mask sampling | 3.098 | 3.234 |
| Rebuild: split into brick geometry | 4.764 | 4.339 |
| Rebuild: duplicate removal and upload | 9.093 | 9.469 |
| Rebuild total | 30.295 | 30.602 |

Subphase medians need not sum to the total median. These diagnostic samples are evidence of where time is spent, not a before/after performance claim. Roughly 24 ms of whole-preview initialization followed by 30 ms of whole-mesh rebuilding explains the remaining delay even with low CPU load. Further work must reduce those phases while preserving complete previews, boundary correctness and all brush actions; postponing the same work to the next event would not meet the goal.


## Shared source-grid samples on updated main

The next engine change evaluates each unique source-grid position once during
eligible full preview initialization, then restores the existing brick sample
layout. On a 13³ grid this removes 27.7% of source evaluations. Against main
`8ab1659d`, the engine materialization probe improves 10.685→8.964 ms for a
one-item source and 214.076→156.149 ms for 32 items, with identical complete
serialized output.

The application comparison keeps desktop `4663b68` unchanged and restricts both
builds to the same performance cores. All 260 paired brush cases complete with
matching uploads. Smooth preparation improves 61.189→57.669 ms; Relax preparation
is nearly flat, and other actions are mixed. CPU idle during the measured sweep
is 85.5% median and 73.9% minimum. A contended partial attempt is excluded.
These current-baseline results do not establish a cross-release trend from the
earlier timings above. Full preview preparation and mesh rebuilding still need
substantial work to meet 16 ms across all actions.

See [source-grid validation](changes/reuse-source-grid-samples/validation.md)
for all brush medians, observed tails, exactness checks and build identities.
