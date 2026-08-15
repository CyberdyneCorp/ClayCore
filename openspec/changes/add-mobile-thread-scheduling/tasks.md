# Tasks: add-mobile-thread-scheduling

- [ ] 1.1 DECIDE and record in `design.md`: the default QoS class, and whether interactive dabs and background refills want one pool or two. Decide against how the app actually drives the library, not in the abstract
- [ ] 1.2 DECIDE and record: how performance cores are counted per platform (`hw.perflevel0.logicalcpu` on Apple), and what the fallback is where the platform does not distinguish them
- [x] 1.3 Baseline on `main`: a dab dispatched from a thread simulating the UI thread, with the CPU time burned in the join-spin measured separately from useful work
- [ ] 1.4 Workers declare a QoS class on Apple platforms; the no-op elsewhere is stated in code rather than silent
- [ ] 1.5 Pool sized from performance cores, host-overridable, zero meaning serial on the calling thread
- [x] 1.6 Replace the yield-spin at the join with a real wait. Preserve the existing guarantee exactly: once done == num_tasks no worker is inside `fn`, and a late-waking worker can never touch a completed call's state
- [ ] 1.7 C ABI: a versioned worker-configuration descriptor plus a query, with an out-of-range value refused rather than clamped
- [ ] 1.8 Test: worker count zero gives results identical to the threaded path, over the golden corpus
- [x] 1.9 Test: "every element of a batch is computed exactly once" still holds under the new join, including the ragged and single-chunk cases
- [ ] 1.10 Stress test the shutdown and late-worker paths, which the shared_ptr job state exists to make safe — the join is being changed underneath them
- [x] 1.11 Measure the join-spin CPU time again after the change, and record it. The claim is that it goes to approximately zero, not that it gets better
- [ ] 1.12 Document in `docs/05-claycore-library.md` that the library spawns a pool at all, and how a host sizes it — today's "the caller owns threading and queues" reads as though it does not

## Nested safety — added after #119's inventory, taken early with the join

The pool is not nested-safe, and that is not in the list above because the list
predates the threading inventory that found it. It is taken here rather than
later because it is the one defect that MUST land before a second subsystem
adopts the pool: brick meshing over independent bricks, whose per-brick work
evaluates through the pool, is two layers of parallelism stacked on a pool that
has one job slot.

- [x] N.1 A `parallel_for` issued from inside one runs inline on the calling
      thread and does not touch the pool.
- [x] N.2 The guard restores the previous nesting state rather than clearing
      it, so a nested inline call does not tell the frame above it that the
      pool is free.
- [x] N.3 Test that FAILS without the guard. Note which one does and which one
      does not: "every element exactly once" still passes without it, because
      the defect is a silent serialization rather than a wrong answer — the
      test that catches it is the one asserting the nested range ran on one
      thread (measured: 187 threads without the guard, 1 with it).

## What this slice deliberately leaves

Taken: the join and nested safety, because #111 is blocked on them. NOT taken,
and still open above: the QoS class (1.4), performance-core sizing (1.5), the C
ABI configuration surface (1.7, 1.8) and the documentation of the pool's
existence (1.12). Those are device tuning and host control; neither is a
prerequisite for parallelising a second subsystem, and bundling them would put
a portable correctness fix behind a platform investigation.

Measured for 1.3 and 1.11, on an unbalanced 4096-element batch over 200 rounds,
where the issuing thread runs out of chunks early and waits:

| | issuing thread's own CPU | wall |
|---|---|---|
| yield spin (before) | 408.5 ms — **99% of the wait** | 412.7 ms |
| condition variable (after) | 3.8 ms — **1%** | 412.1 ms |

Wall time is unchanged, which is the point: the core comes back and the batch
does not get slower.
