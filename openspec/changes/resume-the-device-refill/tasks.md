# Tasks

- [x] Measure both shapes the issue names against the real C ABI on a real
      adopted device, and record the crossover, before choosing one (`design.md`).
- [x] Add `Backend::write_device_buffer` / `read_device_buffer` and
      `BackendCaps::device_copy`, defaulting to `Unsupported` and false so every
      backend without them falls back to the full walk.
- [x] Implement both on the Vulkan backend through a copy SHADER — a caller's
      buffer carries no transfer usage we may assume — with binding 2 gaining
      the caller override bindings 3 and 4 already had.
- [x] Extract the per-brick resume out of `clay_brick_cache_eval_requests` into
      one `resume_one_brick` both refill entry points drive, so the two cannot
      compute different fields.
- [x] Resume in `clay_brick_cache_eval_requests_device`: resumed bricks written
      into their slots per contiguous run, un-resumed bricks evaluated on the
      device per contiguous run and read back to become seeds.
- [x] Store no seed where this path cannot say what one would mean — more than
      one visible SDF layer, or a backend that cannot copy.
- [x] Regression test: a stroke over a window primed in the MIDDLE, so a
      resumed run and two un-resumed runs are in flight at once; parity against
      a document rebuilt from the same items and refilled with no seed at all,
      to the parity suite's 1e-4; and `clay_document_resume_stats` as the
      witness that the fast path fired.
- [x] Device-independent test that the capability and both operations default to
      refusing, which is what makes the fallback a contract rather than an
      accident.
- [x] Prove each test fails with its fix reverted, and that the revert compiles.
- [x] Document the device resume, the copy primitive and the declined shape in
      `docs/05-claycore-library.md`.
- [ ] Metal: no seeded path, falls back. Filed device-gated as #350.
