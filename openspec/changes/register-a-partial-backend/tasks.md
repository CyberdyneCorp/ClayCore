# Tasks: register a partial backend

## 1. The evaluation interface

- [x] 1.1 `BackendCaps` gains a flag per core operation (`eval_points`,
      `eval_grid`, `raycast`), each defaulting to `true` so every existing
      backend keeps reporting what it already provides without being edited.
- [x] 1.2 A registry-level sink records why a compiled-in backend did not
      register, and distinguishes that from a backend absent at compile time.
- [x] 1.3 The sink is readable without constructing a document or a device, and
      forces registry construction so "nobody has tried yet" cannot be mistaken
      for "nothing went wrong".

## 2. The Metal backend

- [x] 2.1 Registration requires the point and grid pipelines only.
- [x] 2.2 A missing raycast pipeline reports `raycast = false` in `caps()` and
      returns `Status::Unsupported` from `raycast()`.
- [x] 2.3 A missing batched-grid pipeline falls back to the base class's loop
      over `eval_grid` rather than refusing, and is not reported as an
      unsupported operation.
- [x] 2.4 A backend that fails to register reports the compiler's own log into
      the sink, not a summary — the same text #59 sends to stderr.

## 3. The C ABI

- [x] 3.1 `clay_backend_op` enum, and `clay_backend_supports`.
- [x] 3.2 `clay_backend_diagnostic`, by the size-query pattern.
- [x] 3.3 Registered in `tools/check_c_abi.py` if it needs to know about the
      enum; parity table updated if pyclay grows a counterpart.

## 4. Tests

- [x] 4.1 A test-only backend registered with raycast unavailable: `caps()`
      reports it, `raycast()` is `Unsupported`, point and grid still match the
      CPU reference. This is what makes the partial path testable on every
      platform rather than only on the device class that motivated it.
- [x] 4.2 The parity suite still passes with such a backend registered — it
      already skips `Unsupported` raycast, so this asserts the existing skip is
      load-bearing rather than incidental.
- [x] 4.3 `clay_backend_supports` through the C ABI: a registered backend, an
      unregistered one (`CLAY_ERROR_NOT_FOUND`), a bad op, null arguments.
- [x] 4.4 `clay_backend_diagnostic`: empty for a healthy backend, non-empty for
      a failed one, size-query and buffer-too-small paths, and the
      never-compiled-in case reading differently from the failed one.
- [x] 4.5 A regression test for the issue's own shape: a backend whose raycast
      is unavailable is STILL in `clay_list_backends`. That is the assertion
      that fails on 0.30.0.

## 5. Docs

- [x] 5.1 `docs/RELEASE.md` release entry: additive, two symbols and one enum.
- [x] 5.2 The backend section of `docs/05-claycore-library.md` describes partial
      registration and what a host should do with an `Unsupported`.
- [ ] 5.3 Issue #63 closed with what was decided, not only what was changed.
