# Tasks: speed-the-metal-path

- [x] 1.1 DECIDED, and the answer is `scene::Tape::compile_id` — process-unique
      per compile, so an id hit IS a content hit and no bytes need hashing
      (which would cost what the upload costs, linear in a consolidated
      volume's blob). Recorded in `metal_backend.cpp`'s resident-tape comment
      rather than in a `design.md`, because this change never had one; the
      collision requirement is met by construction rather than by measurement.
      A tape with no identity (compile_id 0, hand-assembled) goes through the
      scratch slots and is re-uploaded per call, exactly as before.
- [ ] 1.2 Baseline on `main`, on Apple silicon: per-brick fill at 8³, and the grid sweep that put the CPU/Metal crossover at 16³. Break the 288 µs down into allocation, upload, dispatch and readback, so the change can be attributed
- [x] 1.3 DONE. `MetalBackend::upload_tape` keeps the last few uploaded tapes
      in `resident_[]` keyed on `compile_id`. `patch-the-resident-tape` then
      went further than this task asked: an APPEND shares a prefix with the tape
      it grew from, so only the suffix is written — before it, every append was
      a guaranteed miss that re-allocated and re-copied 7.82 MiB per stamp to
      absorb ~148 bytes.
- [x] 1.4 DONE. `scratch(Slot, bytes)` grows a per-slot buffer to the largest
      recent request and reuses it; every slot is released in the destructor.
      Held by "pooled batch scratch survives interleaved batch sizes" in
      `test_backend_residency.cpp`.
- [ ] 1.5 Avoid the copies unified memory does not need: `bytesNoCopy` for inputs where alignment permits, read results from `contents()` in place. Keep the copying path for callers whose memory cannot be wrapped, and state which callers those are
- [x] 1.6 DONE. The four-tap tetrahedron is dispatched as ONE batch of 4n
      points through the kernel the distances already use — no second pipeline
      and no new MSL, because a tap is an ordinary point. The taps, the weighted
      sum and the normalize are written out as four arrays exactly as
      `tape_block.cpp` writes them for the CPU's blocked path.
      **3442 ms -> 37.3 ms** for 20,000 points over a 2,000-item document, which
      also takes it from 23x SLOWER than the CPU backend to 4x faster.
      Two things found while doing it: nothing in the suite compared gradients
      on any backend (so the old fallback's cost was invisible), and Metal
      REFUSED a gradient-only query the interface documents. Both fixed.
- [x] 1.7 RESOLVED THE SECOND WAY: the spec is amended and `device_meshing`
      stays false, because the CODE was the honest side of the contradiction.
      `MetalBackend::mesh` is a hybrid — values from its own `eval_grid`,
      triangulation on the host through `grid_mesh` — and the flag correctly
      said so while `evaluation-backends` required "on-device meshing".
      Why not the other way: topology is fully determined by the evaluated
      values, so a device triangulator would produce the same mesh while being a
      second implementation of the step most able to drift — the reason
      `BrickCache::submit` keeps quantization and band classification off the
      device as well. And the flag is load-bearing: `test_vulkan.cpp` asserts it
      false with the note that a caller who trusts it and gets host meshing is
      measuring the wrong thing.
- [x] 1.8 HOLDS. `test_parity.cpp` compares every registered backend against
      the scalar reference and reports `PARITY_BACKENDS_CHECKED: cpu,metal` on a
      Metal build; worst relative error 0. `ship-metal-in-the-xcframework` task
      1.10 added a document-BAKED volume to that corpus, which is the one shape
      it had been missing.
- [x] 1.9 DONE, in `test_backend_residency.cpp`: "re-evaluating one tape is
      byte-identical call after call", "a recompiled document with same-sized
      sections is never served stale", "evaluation stays correct through
      eviction and re-upload", and the pooled-scratch case above. The
      same-sized-sections one is the sharp one — it is the case a residency key
      could get wrong without any size changing to give it away.
- [x] 1.10 DONE. `docs/RELEASE.md` retired the "keep refill on cpu" advice at
      0.28.0 (#64), and the measured routing rule is now published in `clay.h`
      where a host will actually read it: at dim 8, below ~16 bricks `"cpu"`
      wins, at a dab's 27 bricks `"metal"` is ~2x ahead, and at 4096+ it is 30x
      — with the instruction to re-measure before hardcoding the threshold on
      anything that is not an M-series Mac.
- [x] 1.11 HOLDS by construction: `raycast` goes through the same
      `upload_tape` and the same `dispatch`/`wait_for` helpers as the grid
      paths, so it takes the residency and the pooling without a second code
      path to regress. `test_metal_tape.cpp` and the parity raycast case cover
      it.

## Reconciled 2026-09-01

Seven of the eleven tasks were already done and none of them was ticked: the
work landed under `patch-the-resident-tape`, `batch-brick-eval` and
`speed-the-tape-prim-path`, each of which ticked its own file. This one read
0 of 11 done while the backend had had tape residency and a buffer pool for
releases, which made #243 describe a backend that no longer existed and would
have had anyone starting here rebuild both.

TWO ARE GENUINELY OPEN. 1.6 and 1.7 were closed on 2026-09-01; the two below
were never touched:

- **1.2** the attribution baseline (breaking the 288 us into allocation,
  upload, dispatch and readback). Superseded in spirit — the two costs it was
  meant to size have since been removed — but no such breakdown was ever
  recorded, so the number is not defensible today either.
- **1.5** `bytesNoCopy` for inputs and reading results from `contents()` in
  place. Every path still `memcpy`s out of a shared buffer.

NEITHER needs Apple hardware to WRITE, though both need it to measure. That is the correction #243 rests on: this change is not "fully
blocked".
