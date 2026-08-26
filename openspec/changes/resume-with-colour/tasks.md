# Tasks

- [x] 1.1 `eval_points_seeded` takes a colour seed and runs the coloured walk when the caller wants colour back and supplied it
- [x] 1.2 Asking for colour back without supplying it returns distances only, rather than folding against black
- [x] 1.3 The seed store keeps both planes; the budget becomes bytes rather than a brick count
- [x] 1.4 The refill serves coloured requests from coloured seeds, and gathers coloured misses into the same batch
- [x] 1.5 A brick refilled without colour cannot serve a coloured request
- [x] 1.6 Unit test: a coloured seeded suffix is the whole document bit for bit, with a check that the colours vary
- [x] 1.7 ABI tests: colour through the resumed path, and a colourless seed falling back
- [x] 1.8 Mutation-test that the coloured resumable path is actually taken
- [x] 1.9 Update `docs/`
