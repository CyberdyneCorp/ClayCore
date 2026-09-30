## Implementation
- [x] Reproduce the issue's table in a C ABI test and confirm it fails on main (1171 vs 192 surface bricks).
- [x] Instrument `touch_region_locked` to confirm pre-append seeds are advanced across the append (rev 3 -> 5).
- [x] Record each append's reach in the log; carry a lagging seed only across appends that miss it.
- [x] Read the log before forgetting it in all three region fronts.
- [x] Pin that a seed no append reached is still resumed (`clay_document_resume_stats`).
- [x] Mutation-check the gate: main-like carry, drop-all, and carry-without-scan each fail at least one case.
- [x] Replace the per-seed log scan with a range-union tree (42.9 ms -> 1.27 ms for the first region edit after 3,000 dabs).
- [x] Pin the log offset across several appends: a later append that reaches a brick holds it back, and an append the brick was refilled after does not. Mutants that read the log from its start, skip one entry, or test only the first entry each fail.
- [x] Update `docs/05-claycore-library.md`.
