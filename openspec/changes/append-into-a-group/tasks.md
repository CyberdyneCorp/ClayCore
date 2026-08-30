# Tasks

Two phases, and which one buys the number is stated because it is not
obvious: phase 1 cannot move `sdf_stroke_in_group_bricks` and should not be
judged by it. See design.md, "Correction 3".

## Phase 1 — the whole-document append (does NOT move the gate)

- [x] 1.1 `TapeCheckpoint` carries the ANCESTOR STACK it was taken in, not a
      single layer: per enclosing group, its op, blend, rounding (layer-scaled)
      and whether it EMITS a combine. Emission is `have_acc || seeded`, the
      rule `compile_group` already applies — an inner `Add` group entered with
      no accumulator emits nothing, which is why "one combine per level" is
      wrong (design.md, "Correction 1")
- [x] 1.2 `run()` records a checkpoint after the deepest TAIL chain — following
      tail position down through groups — rather than only after a layer's root
      list
- [x] 1.3 `resume()` re-emits exactly the pending combines a full compile would
      have emitted, in order, and applies the same `tape.info` updates
      (`cfi_extended_blend` / `cfi_smooth_blend`) each one carries
- [x] 1.4 `tail_append` accepts a tail append into a group whose ancestors are
      all in tail position; every other shape keeps today's refusal — an insert
      short of the end at any level, a group not last among its siblings,
      shared content, a non-local combine above the append
- [ ] 1.5 Test: a 24-dab stroke into a group compiles a tape byte-identical to
      a full compile of the same document, dab by dab, at depths 1 and 2.
      PROVED OUT already with a probe over seven shapes — root list, one Add
      group, two and three nested, Subtract, hard-blend, nested Subtract — 8
      appends each: 6 shapes byte-identical with zero mismatches, and the
      seventh REFUSED rather than wrong (a nested Subtract group entered with
      nothing beneath it is skipped whole by the compiler, so the tail path
      never reaches it and there is no checkpoint to resume from). Needs
      writing up as a real test
- [ ] 1.6 Test: the append path actually FIRES for that stroke, asserted
      through the counters — a fast path that silently stopped firing reads as
      correct
- [ ] 1.7 Test: every refused shape still refuses, and each for its own reason
      rather than incidentally
- [ ] 1.8 Benchmark the whole-document compile, and record that
      `sdf_stroke_in_group_bricks` is UNCHANGED by this phase. A flat device
      number here is the expected result, not a failure

## Phase 2 — the per-brick resume (this is what moves the gate)

- [ ] 2.1 The resume's split point becomes a PATH rather than a root-list
      ordinal. Today `compile_layer_prefix` takes `roots.begin() .. +count`,
      `compile_layer_suffix` checks the tail of `roots`, and
      `root_ordinal_of` / `prefix_boundary` / `dirty_from` are all ordinals
- [ ] 2.2 The seed's boundary and the C ABI's resume bookkeeping
      (`clay_internal_resume_frontier`, `touch_appended`'s append log) carry
      the path; the seed key changes shape, so a seed from before this cannot
      be mistaken for one after it
- [ ] 2.3 `compile_layer_suffix` reproduces `compile_group`'s ROLLBACK: a group
      whose children all miss the brick's cull region compiles to nothing, and
      a resumed append into it must emit no combine either (design.md,
      "Correction 2" — this binds the culled path only, since
      `compile_document_append` passes no cull)
- [ ] 2.4 Test: per-brick culled tapes over a stroke into a group are
      band-clamp identical to the full tape, including the fully-culled-group
      case
- [ ] 2.5 Re-run `sdf_stroke_in_group_bricks` on the reference iPad. It should
      approach `sdf_stroke_bricks` (0.034 ms/dab) and go FLAT across the axis;
      today it is 3.07 ms/dab at 1000 items and grows
- [ ] 2.6 Re-derive its budget from the fixed run — 109.5 ms was a ceiling for
      a case growing with the document, and keeping it would be a budget too
      loose to fail
- [ ] 2.7 `docs/09-brush-latency-and-coverage.md` — the pair, and what the
      ratio was before
