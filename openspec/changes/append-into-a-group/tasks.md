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
- [x] 2.1b-ii DONE, and it was two bugs rather than one. See the commit: the
      stack had to live BESIDE `values` rather than in place of it (a brick at
      the current revision is answered straight out of `values`, and a stack is
      not the field), and `store_seed` had to clear a stale stack (a full walk
      produces none, and leaving one paired it with a fresh field -- visible as
      a BAND of document sizes where the cull pad steps mid-stroke). Verified
      exact across 20..1000 stamps
- [x] 2.1b-ii-perf THE STACK WAS BEING DROPPED. Every consumer sized a
      checkpoint's stack `frames.size() + 1`, but a group emits a combine only
      where there is something to combine with -- `TapeCheckpointFrame::emits`,
      which the header had already warned about and which four sites then
      ignored. The refill asked for one shape, the walk reported another, and
      the stack was discarded, so every dab paid the full walk the resume
      exists to avoid. SILENT: a dropped stack is slow, not wrong, which is why
      the sweep stayed exact through all of it. `checkpoint_stack_levels()` is
      now the one rule and the four sites call it. Measured over a 24-dab
      stroke: 6908 of 25660 brick tasks rebuilt and ZERO stored a usable stack;
      now 1040 rebuild and all 1040 store one. 23.0 -> 1.56 ms/dab at 1000.
      Then one walk where there were two -- `eval_points_stack` computed the
      field (the top of the stack IS the field) and discarded it, so the
      rebuild walked the active half again to get it back -- 1.56 -> 1.15 ms.
      That also gives the rebuild a COLOUR stack, which it never stored, so a
      coloured group stroke could not have converged even with the count fixed.
      Regression test `tests/unit/test_checkpoint_stack_levels.cpp` checks the
      claimed count against the walk's actual depth; it fails 5 assertions
      against the old rule, `1 == 5` on four nested Add groups
- [x] 2.1b-ii-perf-b SEEDED AT THE REFILL. A brick's first seed comes from a
      full refill, and for a root-list append the field IS that seed, which is
      why a root-list stroke never paid for a brick's first touch. Inside a
      group the seed is the group's own chain, which the combine has folded
      away by the time the field exists -- so the brick re-walked the whole
      active half on first touch. The refill already compiles a per-brick
      culled tape, so it can take the checkpoint too, and the walk that makes
      the field is the walk that passes it: fused, the stack is free. Where the
      checkpoint sits at the tape's end with nothing open above it -- half the
      bricks -- the field IS the plane and no walk is needed at all. Only taken
      where the batch was going to run on the CPU anyway; a GPU grid batch
      produces no stack and a second CPU pass would cost more than it saves.
      Two things fell out. (1) A seed's frames are the BRICK's, so nothing
      batch-wide can vouch for them and the plan states none by design; read
      for an append to the ROOT list, a seed taken while the tail was a group
      makes the suffix compile refuse, and a root-list stroke over a grouped
      document fell to 0.61 ms/dab from 0.045. The check comes from the append
      target itself -- the enclosing groups of the appended nodes are what a
      checkpoint's frames name. (2) An EMPTY group compiles to nothing and is
      rolled back, so no checkpoint was recorded where the first stroke into a
      newly made group would land. The position in front of the rolled-back
      chain is still a position; what differs is that the chain has produced
      nothing, so `layer_have_acc` is false, the stack stops at the plane
      OUTSIDE the group, and the first dab GROWS it by one -- which the resume
      now expects rather than rejects. Rebuilds over the probe 1040 -> 368.
      Regression test `test_checkpoint_stack_levels.cpp`
- [ ] 2.1b-ii-perf-c WHAT REMAINS, and it is bounded: a first stroke into a
      group that was EMPTY at the last full refill still pays one full walk per
      brick on first touch, because that brick's seed predates the group.
      1.06 ms/dab at 1000 items and 3.04 at 3000 -- both under the 4.17 ms
      interactive frame share, where before this change 3000 was over it. A
      stroke into a group that HAS content pays nothing: 0.172 ms/dab at 1000.
      NOTE the reverted wrong turn, which still stands as a warning: skipping
      bricks the dab "cannot reach" used `node_influence_bound_in_document`,
      the node's own reach rather than its reach through the group's blend, so
      it skipped bricks the combine still moves (errors at 20-80 stamps) and
      bought no speed. The right bound is `node_reach_bound`, and "no frames"
      is not the same thing as "cannot reach"
- [ ] 2.1b-iii ORIGINAL NOTE (superseded): See design.md "Correction 6": a
      frame's `emits` depends on whether anything before the group survived
      THAT BRICK's cull, so one plan cannot state it for a batch. ResumeEntry
      gains the frames it was taken with and the suffix uses those rather than
      the plan's; the plan keeps only `appended`, which is batch-wide. This is
      the last piece, and it is a design change rather than a fix
      the wiring, attempted twice and reverted twice. See design.md "Correction 5":
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
