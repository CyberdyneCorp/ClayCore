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
- [x] 1.5 Test: a 24-dab stroke into a group compiles a tape byte-identical to
      a full compile of the same document, dab by dab, at depths 1 and 2.
      PROVED OUT already with a probe over seven shapes — root list, one Add
      group, two and three nested, Subtract, hard-blend, nested Subtract — 8
      appends each: 6 shapes byte-identical with zero mismatches, and the
      seventh REFUSED rather than wrong (a nested Subtract group entered with
      nothing beneath it is skipped whole by the compiler, so the tail path
      never reaches it and there is no checkpoint to resume from). Needs
      written up in test_tape_prefix_reuse.cpp, beside the root-list cases and
      sharing require_identical -- which checks `info` and `bounds` too, not
      just the three byte arrays, so a tape that matches byte for byte while
      folding a different Lipschitz bound is caught
- [x] 1.6 Test: the append path actually FIRES for that stroke — the 24-dab
      case asserts `compile_document_append` returned true on every dab and
      that each tape's `parent_id` names the one before it, so a path that
      silently stopped firing fails rather than reading as correct
- [x] 1.7 Test: a group not in tail position, and an insert short of the end of
      a group, are both refused
- [ ] 1.8 Benchmark the whole-document compile, and record that
      `sdf_stroke_in_group_bricks` is UNCHANGED by this phase. A flat device
      number here is the expected result, not a failure

## Phase 2 — the per-brick resume (RE-SCOPED, see design.md "Correction 4")

Phase 1 landed and, exactly as predicted, moved the brick path not at all:
2.48 ms/dab against 0.043 at 1000 items, unchanged. That is the design working,
not a failure -- and building it is what showed phase 2 to be a bigger change
than this file first described.

The per-brick resume seeds ONE value per sample. A split inside a group needs
one per open group plus one, which reaches past the compiler into
`eval_points_seeded`, the kernel's tape evaluation, the seed store's per-brick
memory and byte accounting, the seed key and the eviction budget. The
associative shortcut does not rescue it: a hard-blend group append still
measures 1.18 ms/dab, because it needs the APPENDED node to be hard Add too
and a sculpting dab is smooth.

**Recommendation: land phase 1 and re-propose phase 2 as its own change**, with
the kernel/eval boundary named in its impact rather than discovered in it.
The tasks below are kept as the record of what it would take.

- [x] 2.1a THE MECHANISM, and it is exact. `eval_points_stack` reads a tape's
      whole final stack; `eval_points_seeded_stack` starts a walk holding N
      values; `snapshot_at` takes the stack mid-walk so one walk produces both
      the answer and the next dab's seed. Verified as a 24-dab chain into a
      group, each dab seeding the next and every dab checked against a full
      compile: worst error 0.0, zero mismatches
- [x] 2.1b-i The cull-aware resumable compiles, which Correction 5 said were
      missing: `compile_document_resumable` and `compile_document_part_resumable`
      now take a cull, index and plan and report the checkpoint. Verified
      byte-identical to the culled compile, and a culled group resume through
      them is exact -- worst 0.0 over 216 points at 40 and 300 items
- [ ] 2.1b-ii THE SEED MUST CARRY ITS FRAMES. See design.md "Correction 6": a
      frame's `emits` depends on whether anything before the group survived
      THAT BRICK's cull, so one plan cannot state it for a batch. ResumeEntry
      gains the frames it was taken with and the suffix uses those rather than
      the plan's; the plan keeps only `appended`, which is batch-wide. This is
      the last piece, and it is a design change rather than a fix
- [ ] 2.1b-iii THE WIRING, attempted twice and reverted twice. See design.md "Correction 5":
      a brick's first seed comes from a FULL refill, which stores the finished
      field as one plane, and a group resume needs the open chains. The full
      refill must store the stack at the checkpoint, which needs a CULL-AWARE
      RESUMABLE COMPILE that does not exist yet -- compile_document_resumable
      takes no cull and the culled compile reports no checkpoint. Do that
      first; everything else in phase 2 depends on it
- [ ] 2.1c The resume's split point becomes a PATH rather than a root-list
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
