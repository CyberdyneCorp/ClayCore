# Validation in progress

## Isolated coordinate prototype

Seven alternating pairs across eight grid configurations (112 runs) compare full output float bits. Each run generates all 729 samples of 2,744 bricks. Scalar generation measures median 7.6–8.1 ms versus 0.9–1.5 ms for a prototype that computes the brick base once. This is an isolated prototype result, not a production Smooth latency measurement.

## Transaction equivalence

A before/after executable comparison links the original `9cc0d181` SdfSourceField implementation and the new implementation against the same otherwise-current core library. Sixteen configurations cover one- and two-sphere fields, two cell sizes, zero/nonzero dab strength and whole-volume priming versus lazy local materialization. Three updates per configuration produce 48 complete snapshots. The serialized preview volume, dirty-brick coordinates/order, preview generation and changed flag match byte-for-byte in all snapshots (49,980,336 bytes per file). Both SHA-256 hashes are `74a313348a0dac1fa253ef1fbc0b29081d7aae0fcfbb627e966581cc7b7f1539`.

## Static checks

Layering and all 65 strict OpenSpec items pass. Clang-tidy reports cognitive complexity 10 for the bulk coordinate writer and 8 for the reference-check helper; the table-driven regression scores 10. The source-fill callback is simpler after replacing its coordinate loop. These counts are within the systems target.

## Pending

- Final exact-coordinate regression, field/prefix-cache/Smooth suites and sanitizers.
- Production coordinate and live Smooth performance measurements.
- Platform CI and final PR documentation.

## CPU verification

The final exact-coordinate regression passes 384 checks, including a window crossing a brick plane boundary. All 11 CPU CTest targets pass (100.71 seconds), including 2,757 C++ cases (16,647,615 assertions) and 756 Python tests with one intentional skip. The complete CPU build, including the shared library and Python module, succeeds. Sanitizer and live performance validation remain pending.

Both Clang and GCC also pass all 112 exact-output comparisons with `-O3 -march=native -ffp-contract=fast`, using the actual grid method definitions. Those concurrent runs are correctness checks only; their timings are not performance evidence. The final table-driven regression still scores 10 in Clang-tidy, the check helper scores 8, and the source-fill callback scores 9.

## Combined host verification

Host `c665af3` built against Core production revision `abd87b0b` passes all 85 enabled combined cases: 68 library and 17 native/rendered integration tests (three command-interface, four sculpt-latency, five settlement, two rendered-brush and three rendered-incremental). No adapter skips occur. The informational library timing test is intentionally ignored. The host engine pin is restored to v0.113.0 after preserving the combined executable. Sanitizer execution and uncontended production/live timing remain pending.

## Sanitizer verification

ASan/UBSan with leak detection passes all 133 selected field, relax, prefix-cache, Smooth transaction and cancellation cases (10,001,774 assertions), including the new coordinate regression. The Debug prefix-cache tests are a documented long-running shard; the selected run completed successfully after roughly 25 minutes. An initial invocation stopped before tests because the sanitizer runtime was behind a preloaded C++ library; the successful run preloaded libasan first.

## Production coordinate measurement

The production helper and scalar reference pass all 112 exact-output comparisons (seven alternating pairs for eight grids, 2,744 bricks per run). Median coordinate generation is 7.108–7.531 ms for scalar calls and 1.296–1.594 ms for the bulk helper. These are isolated algorithm timings. No builds or tests from this task ran concurrently, although the machine's one-minute load was still decaying from earlier work (about 15 at the start); these figures do not establish application latency.

## First live comparison and unresolved release cost

Three alternating application pairs across all 13 tools complete 78 cases, using identical host production code with Core `9cc0d181` and `abd87b0b`. One-minute load remains 2.594–2.764 on 24 logical CPUs, with no concurrent builds or tests from this task. All paired actions report identical uploaded byte counts. The fixture is a fresh sphere isolated by undo, begin at (0,0,1), continue at (0.12,0,1), end, pressure 1, CPU fields and RTX 5060 Vulkan rendering.

| Tool | Before begin ms | Bulk begin ms | Before end ms | Bulk end ms |
|---|---:|---:|---:|---:|
| mask | 6.152 | 5.855 | 7.772 | 7.669 |
| crease | 1.260 | 1.204 | 21.892 | 23.572 |
| clay | 2.664 | 2.703 | 21.406 | 18.707 |
| inflate | 2.888 | 2.796 | 23.218 | 22.934 |
| layer | 1.242 | 1.332 | 20.052 | 21.673 |
| standard | 1.268 | 1.413 | 21.914 | 21.622 |
| polish | 0.024 | 0.024 | 42.682 | 41.870 |
| planar | 0.033 | 0.021 | 42.479 | 36.985 |
| move-topological | 0.027 | 0.027 | 51.329 | 49.737 |
| move | 0.030 | 0.022 | 63.370 | 63.385 |
| relax | 98.792 | 91.592 | 84.981 | 86.655 |
| smooth | 89.293 | 81.147 | 84.190 | 90.064 |
| snake-hook | 0.026 | 0.028 | 31.107 | 31.447 |

Smooth pointer-down improves in each pair, and Relax improves by median. However, Smooth release is slower in all three pairs; its complete begin-plus-end time is nearly unchanged in the paired observations. This does not establish an overall Smooth interaction improvement or a 16 ms result. Further measurement is needed to distinguish a release regression from scheduling or run variation.

A planned ten-pair focused Smooth/Relax/Standard comparison was interrupted after 45 completed cases when heavy background compilation invalidated its controls (load 9.838–23.496, Standard beginning in several milliseconds and some Smooth calls over a second). Those partial measurements are contended diagnostics, not evidence resolving the release question. No additional production change was made in response to those timings.

Inspection of GCC Release objects finds identical instruction bytes and sizes for the existing FieldVolume eval_color, eval_inside, eval and sample_at routines against a separately compiled original 9cc0d181 volume.cpp. Their text offsets shift by 0x380 bytes after adding the helper. This rules out extra instructions inside those routines, but does not rule out code-placement/cache or application scheduling effects; the live release question remains unresolved.
