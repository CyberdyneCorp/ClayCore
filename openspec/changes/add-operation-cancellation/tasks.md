# Tasks: add-operation-cancellation

## 1. Measure before designing

- [x] 1.1 Confirm the surface is absent rather than deferred: `cancel`,
      `progress` and `interrupt` appear nowhere in `bindings/c/clay.h`, and
      nowhere in `openspec/ROADMAP.md`
- [x] 1.2 Confirm the ABI has no function pointers today — the grep for
      `(*name)(` over `clay.h` returns nothing — so a callback would be the
      first one every FFI consumer has to marshal
- [x] 1.3 Confirm the operations and their cost from `tests/device/baseline.json`
      (ABI 0.39.0, iPad15,5, iOS 26.5.2): `mask_extrude` 4403 ms,
      `sdf_consolidate` 661 ms, `volume_relax` 358 ms, `volume_hpolish` 153 ms,
      `volume_flatten` 152 ms, `mask_extract` 97 ms
- [x] 1.4 Confirm the pool's join rule: `run()` increments `done` only after
      `job.fn` RETURNS, and `parallel_for` waits on `done >= num_tasks`, so a
      cancelled chunk must return normally and must never throw
- [ ] 1.5 MEASURE the checkpoint cost before choosing a granularity: a relaxed
      atomic load per brick window against per sample, on the consolidate
      fixture. This library has been caught assuming a cost is negligible;
      publish the number in `design.md`

## 2. Decide

- [ ] 2.1 DECIDE and record: the entry points that take a token in this slice.
      The six measured operations are the obvious set; `clay_document_mesh` is
      an "export" by the harness's own definition and is not measured
- [ ] 2.2 DECIDE and record: is progress always written, or opt-in? Two relaxed
      stores per checkpoint, paid whether or not anyone reads them. Answer from
      task 1.5's measurement, not from intuition
- [ ] 2.3 DECIDE and record: may one token serve two concurrent operations?
      Forbidding it is one owner field and one sentence; allowing it leaves
      progress with no single meaning
- [ ] 2.4 DECIDE and record: is a token reusable after a cancel, or is it
      one-shot? A host holding one token per document and cancelling once must
      not pay an allocation per cancel on the interactive path
- [ ] 2.5 DECIDE and record: the pyclay shape. A context manager is Python's
      natural form and a cancelling thread is not; the GIL makes the polling
      story different from C, and parity is required

## 3. Build

- [ ] 3.1 `clay_cancel_token` — create / cancel / destroy, an atomic flag and
      the progress state, with the header stating that cancel and read are the
      only cross-thread-legal calls in the ABI
- [ ] 3.2 `CLAY_ERROR_CANCELLED = 9`, appended to the result enum, with no
      error detail message describing a fault
- [ ] 3.3 `clay_progress` as a versioned output descriptor filled through
      `write_desc`, per the `struct_size` rule in both directions
- [ ] 3.4 Checkpoints at the window boundaries that already exist:
      `FieldVolume::sample_blocks`, `field::redistance`, `fill_colors_blocks`,
      and each axis pass of the mask-extrude distance transform
- [ ] 3.5 Phase reporting for `consolidate_layer`, which is six phases and must
      not pretend to be one
- [ ] 3.6 Thread the token through the C++ layer as a small non-owning observer
      type, NOT as `std::function` — the pool's inline-nesting rule and the
      no-throw rule both live below it

## 4. Prove it

- [ ] 4.1 The scenarios in both spec deltas
- [ ] 4.2 The atomicity test, which is the regression for the whole change:
      probe the document, cancel a consolidate in EACH phase, probe again, and
      assert bit-identical evaluation and an unchanged undo depth every time
- [ ] 4.3 A no-hang test: cancel a pooled operation and assert `parallel_for`
      joins. This is the failure mode task 1.4 identifies and the only one that
      is a hang rather than a wrong answer
- [ ] 4.4 Assert a null token is bit-identical to today over the golden corpus,
      so a host that ignores this change is genuinely unaffected
- [ ] 4.5 Cancel before the call and after the call; both are ordinary

## 5. Reach it and say it

- [ ] 5.1 pyclay, so `tools/check_binding_parity.py` stays clean
- [ ] 5.2 Swift smoke coverage, since the C ABI grows
- [ ] 5.3 ABI minor bump, and `docs/RELEASE.md`
- [ ] 5.4 `docs/05-claycore-library.md`: the three budget classes and what a
      host does about the third. The class exists in the device harness and is
      described nowhere a host reads
- [ ] 5.5 A numbered example that cancels a real consolidate and asserts the
      document is unchanged, per the examples capability gate
- [ ] 5.6 `openspec/ROADMAP.md`: this row does not exist in it yet
