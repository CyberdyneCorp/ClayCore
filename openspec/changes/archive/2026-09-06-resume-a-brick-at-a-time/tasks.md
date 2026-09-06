# Tasks

- [x] Replace the batch-wide seed admission test with a per-brick one, memoizing
      one resume plan per distinct stored revision.
- [x] Move the lattice-shape checks into a single `shaped_entry` both lookups use.
- [x] Refuse a resume plan whose append log names a layer other than the one the
      suffix would extend.
- [x] Add `clay_resume_stats` / `clay_document_resume_stats`, ABI 0.55.0.
- [x] Regression tests: a moving window, a batch with one unseeded brick, a batch
      whose bricks were stamped by different dabs, bit-identical parity against a
      document that never resumed, and the cross-layer id collision.
- [x] Prove each test fails with its fix reverted, and that the revert compiles.
- [x] `BM_BrickRefillMoving5000` / `BM_BrickRefillMoving20000`, reporting the
      share of bricks the fast path answered.
- [x] Bump CMakeLists.txt, pyproject.toml and CLAY_ABI_* to 0.55.0.
