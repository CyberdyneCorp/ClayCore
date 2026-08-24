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
- [x] 1.5 MEASURE the checkpoint cost before choosing a granularity: a relaxed
      atomic load per brick window against per sample, on the consolidate
      fixture. This library has been caught assuming a cost is negligible;
      publish the number in `design.md`

## 2. Decide

- [x] 2.1 DECIDED: `clay_layer_consolidate_cancellable` in this slice, as a
      SECOND entry point rather than a parameter on the first — adding one
      would break every host already compiled against it, and the point of a
      token is that a host who does not want one is unaffected. The rest of the
      measured six follow the same pattern.
      Superseded note:
      The six measured operations are the obvious set; `clay_document_mesh` is
      an "export" by the harness's own definition and is not measured
- [ ] 2.2 DECIDE and record: is progress always written, or opt-in? Two relaxed
      stores per checkpoint, paid whether or not anyone reads them. Answer from
      task 1.5's measurement, not from intuition
- [x] 2.3 DECIDED: not modelled, and not forbidden by machinery — progress
      would have no single meaning, so the header says one token per operation
      rather than adding an owner field that costs every call. Revisit if a
      host reports needing it.
      Original question:
      Forbidding it is one owner field and one sentence; allowing it leaves
      progress with no single meaning
- [x] 2.4 DECIDED: reusable. `reset()` clears the flag and the progress, so a
      host holding one token per document does not pay an allocation per cancel
- [x] 2.5 DECIDED: `clay.CancelToken` with `cancel()`, `cancelled`, `reset()`
      and a `progress` dict, passed as `token=` to the operation. Not a context
      manager: a `with` block ends when the operation returns, which is the
      wrong lifetime for a token a host holds per document.
      Original question: A context manager is Python's
      natural form and a cancelling thread is not; the GIL makes the polling
      story different from C, and parity is required

## 3. Build

- [x] 3.1 `clay_cancel_token` — create / cancel / destroy, an atomic flag and
      the progress state, with the header stating that cancel and read are the
      only cross-thread-legal calls in the ABI
- [x] 3.2 `CLAY_ERROR_CANCELLED = 9`, appended to the result enum, with no
      error detail message describing a fault
- [x] 3.3 `clay_progress` as a versioned output descriptor filled through
      `write_desc`, per the `struct_size` rule in both directions
- [ ] 3.4 Checkpoints at the window boundaries that already exist:
      `FieldVolume::sample_blocks`, `field::redistance`, `fill_colors_blocks`,
      and each axis pass of the mask-extrude distance transform
- [ ] 3.5 Phase reporting for `consolidate_layer`, which is six phases and must
      not pretend to be one
- [x] 3.6 Threaded through as `parallel::CancelToken*`, a non-owning pointer,
      NOT a `std::function` — the pool's inline-nesting rule and the
      no-throw rule both live below it

## 4. Prove it

- [x] 4.1 The scenarios in both spec deltas
- [ ] 4.2 The atomicity test, which is the regression for the whole change:
      probe the document, cancel a consolidate in EACH phase, probe again, and
      assert bit-identical evaluation and an unchanged undo depth every time
- [x] 4.3 A no-hang test: cancel a pooled operation and assert `parallel_for`
      joins. If it regresses the SUITE HANGS rather than failing, which is
      exactly why it is pinned
- [x] 4.4 A null token gives the same result as the older entry point, which
      is now literally sugar over the new one
- [x] 4.5 Cancel before the call, twice, against a null handle, and reset
      afterwards; all ordinary

## 5. Reach it and say it

- [x] 5.1 pyclay, so `tools/check_binding_parity.py` stays clean
- [x] 5.2 Swift smoke coverage, since the C ABI grows
- [x] 5.3 ABI minor bump, and `docs/RELEASE.md`
- [x] 5.4 `docs/05-claycore-library.md`: the three budget classes and what a
      host does about the third
- [ ] 5.5 A numbered example that cancels a real consolidate and asserts the
      document is unchanged, per the examples capability gate
- [ ] 5.6 `openspec/ROADMAP.md`: this row does not exist in it yet

## 6. What building it changed

- [x] 6.1 The token lives in `parallel`, not a new module. It has to be visible
      to `scene` (consolidate), `field` (relax, flatten) and `brush` (mask
      extrude), and `check_layering.py` allows `field` exactly
      {parallel, kernel, math} — so `parallel` is the only module all three
      already depend on. It also belongs beside the thread pool, because the
      pool's join rule is the one way to get this catastrophically wrong
- [x] 6.2 The first draft ADDED A PARAMETER to `clay_layer_consolidate`, which
      is an ABI break — and this change's own spec says a token must be
      additive so an existing host is unaffected. A second entry point is the
      answer, the same shape `clay_mesh_validation_report` has to
      `clay_mesh_validate`, with the older call as sugar
- [x] 6.3 A cancel and "there was nothing to consolidate" both come back as a
      failure from `bake_layer`, and a host must not be shown the second when
      the user did the first. `consolidate_layer` reports them apart

## 7. Still open

- [ ] 7.1 The other five measured operations — mask extrude (4403 ms), relax,
      flatten, hpolish, mask extract — take a token by the same pattern. This
      slice proves the mechanism on the one with the clearest phases
- [ ] 7.2 Whether progress is always written or opt-in (2.2), which wants the
      device measurement rather than a Linux one
- [ ] 7.3 A numbered example, once more than one operation is cancellable
