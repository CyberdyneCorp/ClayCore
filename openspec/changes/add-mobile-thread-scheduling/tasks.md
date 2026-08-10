# Tasks: add-mobile-thread-scheduling

- [ ] 1.1 DECIDE and record in `design.md`: the default QoS class, and whether interactive dabs and background refills want one pool or two. Decide against how the app actually drives the library, not in the abstract
- [ ] 1.2 DECIDE and record: how performance cores are counted per platform (`hw.perflevel0.logicalcpu` on Apple), and what the fallback is where the platform does not distinguish them
- [ ] 1.3 Baseline on `main`: a dab dispatched from a thread simulating the UI thread, with the CPU time burned in the join-spin measured separately from useful work
- [ ] 1.4 Workers declare a QoS class on Apple platforms; the no-op elsewhere is stated in code rather than silent
- [ ] 1.5 Pool sized from performance cores, host-overridable, zero meaning serial on the calling thread
- [ ] 1.6 Replace the yield-spin at the join with a real wait. Preserve the existing guarantee exactly: once done == num_tasks no worker is inside `fn`, and a late-waking worker can never touch a completed call's state
- [ ] 1.7 C ABI: a versioned worker-configuration descriptor plus a query, with an out-of-range value refused rather than clamped
- [ ] 1.8 Test: worker count zero gives results identical to the threaded path, over the golden corpus
- [ ] 1.9 Test: "every element of a batch is computed exactly once" still holds under the new join, including the ragged and single-chunk cases
- [ ] 1.10 Stress test the shutdown and late-worker paths, which the shared_ptr job state exists to make safe — the join is being changed underneath them
- [ ] 1.11 Measure the join-spin CPU time again after the change, and record it. The claim is that it goes to approximately zero, not that it gets better
- [ ] 1.12 Document in `docs/05-claycore-library.md` that the library spawns a pool at all, and how a host sizes it — today's "the caller owns threading and queues" reads as though it does not
