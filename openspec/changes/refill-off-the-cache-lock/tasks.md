# Tasks

- [x] Measure first: split the resumed refill's call into cull-index, per-brick
      compile, point build and seeded walk at window sizes 1/4/12/48 and suffix
      lengths 1 and 16, to find what is actually under the lock.
- [x] Measure the thread pool's dispatch cost against the per-brick work, to
      decide whether the pool is worth reaching for at all and where.
- [x] `BM_BrickRefillWindow`, parameterised by window size, history depth and
      appends between refills.
- [x] Split the resumed loop into resolve-and-copy under the lock, compile and
      evaluate released, store under the lock again.
- [x] Run the seeded walk IN PLACE from the buffer the seed was copied into
      (#306 open question 7), so the copy costs no extra pass.
- [x] Re-check the revision before storing, as `store_seeds` does.
- [x] Take `clay::parallel::ThreadPool` for the deferred phase, gated on a
      measured count of sample-instructions.
- [x] Regression test: a refill racing readers, values bit-identical to a
      document that never resumed, both window shapes exercised.
- [x] Prove that test catches the hazard the copy exists to prevent — revert the
      copy, leave the raw seed pointer live across the unlock, and show
      ThreadSanitizer reports the race. Confirm the revert compiles.
- [x] Full suite green under `cpu-only`, `asan-ubsan` and a ThreadSanitizer
      build.
- [x] docs/05-claycore-library.md.
