## Implementation
- [x] Reproduce the issue's table in a C ABI test and confirm it fails on main (1171 vs 192 surface bricks).
- [x] Instrument `touch_region_locked` to confirm pre-append seeds are advanced across the append (rev 3 -> 5).
- [x] Record each append's reach in the log; carry a lagging seed only across appends that miss it.
- [x] Read the log before forgetting it in all three region fronts.
- [x] Pin that a seed no append reached is still resumed (`clay_document_resume_stats`).
- [x] Mutation-check the gate: main-like carry, drop-all, and carry-without-scan each fail at least one case.
- [x] Update `docs/05-claycore-library.md`.
