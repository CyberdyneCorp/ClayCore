# Tasks: batch-brick-eval

## 1. Find out where the time goes before changing anything

- [x] 1.1 Establish that evaluation is the term worth attacking: 167.5 ms of a
      170 ms stamp at 50,000 items, against 2.96 ms of cull-index rebuild and
      0.22 ms of tape compile
- [x] 1.2 Measure CORE UTILISATION rather than assuming the pool was the answer.
      CPU time over wall time, taken inside the evaluate call so serial setup
      elsewhere cannot flatter it: 6.69 cores of 16 physical, falling from 8.85
      at a hundred items
- [x] 1.3 Rule out the measurement itself before trusting it. The pool sleeps on
      a condition variable rather than spinning (`thread_pool.h` says so and the
      worker loop confirms it), so idle workers burn no CPU and the figure is
      not inflated by a busy-wait
- [x] 1.4 Find the cause by reading the dispatch rather than guessing at it:
      `Backend::eval_grid_batch`'s default loops `eval_grid` per brick, and
      `eval_grid` splits a grid's Z-SLICES — of which a brick has eight

## 2. One dispatch for the batch

- [x] 2.1 `eval_row` — one lattice row, the unit both paths dispatch, so there
      is one piece of inner-loop code and not two that could drift
- [x] 2.2 `eval_grid` over rows instead of z-slices
- [x] 2.3 A CPU override of `eval_grid_batch`: one `parallel_for` over every row
      of every brick

## 3. Prove it

- [x] 3.1 Bit-identity between the batch and the same grids one at a time,
      distances and colours, `==` rather than a tolerance — the invariant the
      override must preserve and the one a later optimisation could break
      quietly
- [x] 3.2 The existing cross-backend parity case still passes, so no device
      backend moved
- [x] 3.3 Re-measure on an idle machine, before and after: 167.5 -> 77.4 ms at
      50,000 items, cores 6.69 -> 17.89

## 4. Left undone, deliberately

- [ ] 4.1 **17.9 of 24 threads is not 24.** This machine is 8 performance cores
      plus 8 efficiency cores; the pool sizes from `hardware_concurrency` and
      treats every worker as interchangeable. What is left on the table is a
      hybrid-aware pool, which is `add-mobile-thread-scheduling` tasks 1.2 and
      1.5 — filed there as an Apple concern, and this is the same problem on a
      desktop
- [ ] 4.2 Per-sample cost is untouched. `add-cpu-simd-path` attacks the same
      98% from the other side, and the two multiply. Worth doing next, and
      worth doing AFTER this: SIMD on a path that reached 6.7 cores would have
      won a third of what it will win now
