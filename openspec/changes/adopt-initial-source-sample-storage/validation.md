# Validation

An isolated prototype adopts the first block and retains per-brick appending for later blocks. Six scenarios over seven repeats compare 143,293,584 exact output bytes, including serialized volumes, sample Lipschitz bounds, tallies, added coordinates and callback sample-count observations. The reference and prototype SHA-256 is `4d0d3915440509713bba90379ea66eac6a86f5318661adc49b8362e13330c041`. Sources and binaries are retained under `/tmp/clay-531-adopt-source-block/`. Timing from this run is excluded because other CPU jobs were active. Production regression and performance verification are pending.

## Production regression and allocation evidence

The tightened allocation regression fails against the previous engine: 2,008,560 requested bytes for a 1,000,188-byte sample payload. With initial block adoption it passes at 1,008,372 bytes, avoiding exactly one complete sample payload allocation. The fixture allows bookkeeping below a two-payload bound; it does not assert an allocator-specific exact count. Full/partial/repeated materialization and callback-observation coverage also passes: two focused cases, 39,503 assertions.

The changed materialization function has cognitive complexity 35 under clang-tidy with macro expansions excluded, at the systems-domain target limit. The new materialization test scores 10 and its fill callback 4. Analysis uses the actual target include paths; preliminary runs with incomplete include paths are excluded. All 69 strict OpenSpec items pass. Full CPU and sanitizer verification remains in progress.
