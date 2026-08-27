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
- [x] Re-measure the gate with 1-12 CONCURRENT refills, against `main` and
      against a never-pool build, before treating the constant as settled: the
      pool has one job slot and a second host thread's dispatch replaces the
      first, which `in_job()` does not guard.
- [x] A `tsan` preset and a CI job, so the spec's "no data race is reported"
      scenario is checked by something. Record that the run needs `setarch -R`.
- [x] State the in-place aliasing contract in `include/clay/eval/backend.h`,
      where `eval_points_seeded` is declared.
- [x] Expose `refilled_frac` from `BM_BrickRefillWindow` and gate it in
      `tools/check_bench.py`, as the moving pair is gated.
- [x] docs/05-claycore-library.md, README.md (the `tsan` preset).
