# Tasks: patch the resident tape

## 1. Establish the shape before building

- [x] 1.1 Confirmed both backends re-upload on every append: Metal misses on
      `compile_id` (`metal_backend.cpp:531`), Vulkan short-circuits its
      `memcmp` on the size change (`vulkan_backend.cpp:490`).
- [x] 1.2 Confirmed Vulkan additionally keeps a FULL CPU-side shadow of the
      tape (`res_instrs_`/`res_params_`/`res_blob_`) — another 7.82 MiB at
      50k items, for a cache that cannot hit once during a stroke.
- [x] 1.3 Confirmed the packing asymmetry: Metal uses three buffers, Vulkan
      packs `params ++ blob` into one with `blob_base = params.size()`, so
      appended params shift the blob.
- [x] 1.4 Confirmed the shader reads `blob_base` as an opaque cursor
      (`clay_kernels.comp.in:27`), so the slack layout needs NO shader or
      descriptor-set change.
- [x] 1.5 Confirmed `ensure()` is exact-fit, so patching alone would still
      reallocate every stamp.
- [x] 1.6 DECIDED (with the user): slack between params and blob, over a
      separate blob binding or a blob `memmove`. Recorded in the proposal.
- [x] 1.7 Confirmed the two existing Vulkan residency tests keep passing under
      id-keyed residency, and that the "same length, different contents" one
      gets stricter rather than weaker.
- [x] 1.8 Verify the Vulkan backend registers and its tests run on this box
      before building anything on it. (14 cases passed on the RTX 5060 during
      scoping; re-confirm against the phase-1 branch.)

## 2. Lineage on the tape

- [x] 2.1 Add `parent_id`, `agree_instrs`, `agree_params`, `agree_blob` to
      `Tape`, documented as a claim about BYTES: below those offsets the two
      tapes are identical. `parent_id == 0` means no lineage.
- [x] 2.2 Fill them in `compile_document_append` from the checkpoint it just
      consumed — the agreement point IS the checkpoint — and nowhere else.
- [x] 2.3 Leave `compile_id` semantics untouched, so a backend that ignores
      lineage misses and re-uploads exactly as today. This is what lets Metal
      follow separately.
- [x] 2.4 Test that a lineage claim is TRUE: read the ancestor's sections and
      assert byte-identity below all three offsets. Not "the field looks
      right" — the bytes.
- [x] 2.5 Test that every other compile entry point reports no lineage.

## 3. Vulkan: patch instead of re-upload

- [x] 3.1 Replace the `memcmp` residency check with an id compare: hit on
      `compile_id`, patch on `parent_id`, upload whole otherwise, and ALWAYS
      upload whole for `compile_id == 0`.
- [x] 3.2 Delete the CPU-side shadow vectors; one `std::uint64_t` replaces
      them.
- [x] 3.3 Advance the resident id to the patched tape's own id, so a STROKE
      chains — without this only the first dab after an upload patches.
- [x] 3.4 Lay the floats buffer out as `[params | slack | blob]` and make
      `blob_base` the params capacity rather than `params.size()`.
- [x] 3.5 Patch the three suffixes into the mapped buffers at their offsets.
- [x] 3.6 Fall back to a full upload and re-pack when params outgrow the
      reserved capacity.
- [x] 3.7 Grow buffers geometrically in `ensure()`, reserving the slack out of
      the grown capacity rather than on top of it.
- [x] 3.8 Add a `tape_patches_` counter beside `tape_uploads_`, exposed the
      same way for tests.

## 4. Prove the patched field is the uploaded field

- [x] 4.1 THE test: evaluate the same appended document through a backend that
      patched its way to it and through one that uploaded it whole, and
      require IDENTICAL values. A patch written to the wrong offset passes a
      counter test and fails this one.
- [x] 4.2 Run that over a stroke of many dabs, not one, so a drift that
      accumulates is caught.
- [x] 4.3 Over a document carrying a blob — strokes, a sampled volume, a gate
      — which is the case the slack layout exists for.
- [x] 4.4 Counter test: a stroke of N dabs is one upload and N patches.
- [x] 4.5 Regression: a tape with no lineage, and one naming an ancestor that
      is not resident, are uploaded whole and evaluate correctly.
- [x] 4.6 Regression: a hand-assembled tape (`compile_id == 0`) is never
      served a resident upload.
- [x] 4.7 Regression: exhausting the slack re-packs and still evaluates
      correctly.
- [x] 4.8 Keep the two existing Vulkan residency tests passing unchanged.

## 5. Measure

- [x] 5.1 Benchmark a stroke through the Vulkan backend: append, evaluate,
      repeat, against the same stroke with patching disabled.
- [x] 5.2 Report the reallocation count as well as the time — the allocator
      churn is half of what #197 is about, and a wall-clock number on a
      desktop with 8 GB of VRAM under-reports what it costs an iPad.
- [x] 5.3 Gate it, named rather than `Arg()`-parameterised, and set the
      ceiling from measured contention rather than from the quiet number —
      the trap phase 1's gate walked into.

## 6. Documentation and the follow-up

- [x] 6.1 Update the `Tape` header: what lineage promises, and that a false
      claim is silent.
- [x] 6.2 Update `docs/05-claycore-library.md`, which currently ends the tape
      section saying the GPU still re-uploads.
- [x] 6.3 Update #197 with the Vulkan result, and state that Metal is the
      remaining half.
- [x] 6.4 Open the Metal follow-up as its own issue rather than leaving it
      implied — it needs a Mac to measure and an iPad to validate the memory
      story. Filed as #296, carrying the three things the Vulkan work learned
      the hard way: patch on parent_id, ADVANCE the resident id or only the
      first dab patches, and reserve slack or the patch declines every stamp.
