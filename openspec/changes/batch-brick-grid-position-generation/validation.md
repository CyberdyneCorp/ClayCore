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
