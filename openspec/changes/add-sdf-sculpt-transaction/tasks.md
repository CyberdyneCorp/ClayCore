# Tasks: add-sdf-sculpt-transaction

## 1. Establish what is already there

- [x] 1.1 `field::relax` smooths a volume and COPIES its input first;
      `make-the-relax-dab-local` already made a regioned pass cost the bricks
      it selects. Neither the algorithm nor the region limit is rebuilt here
- [x] 1.2 `scene::consolidate_layer` bakes a layer and installs the result,
      severing shared instance content and recording one undoable step. The
      install half is not reachable on its own
- [x] 1.3 `brush::move_brush` resolves a world drag into per-item warps and is
      pure; `moved_chain` owns the front-of-chain rule and the leading-grab
      replacement from `add-move-drag-continuity`
- [x] 1.4 `scene::report_layer` measures `safe_step_scale`,
      `longest_deformer_chain` and `item_count` and deliberately never bakes —
      `add-consolidation-policy` settled that and it is unchanged
- [x] 1.5 `UndoStack::begin_group`/`end_group` bundle commands into one step,
      one bracket deep
- [x] 1.6 `session/` already exists (`session/history.h`) and is where state
      that lives for a sitting and is never saved belongs

## 2. `relax_in_place` — the in-place half of relax

- [x] 2.1 `field::RelaxResult { dirty_bounds, touched_bricks, changed,
      cancelled }`
- [x] 2.2 `relax_in_place(volume, settings, token)` smooths the caller's volume
      and returns the result
- [x] 2.3 `relax` delegates over a copy, so there is ONE algorithm; it still
      returns its INPUT on cancel, which is its own contract and not this one's
- [x] 2.4 Cancellation checkpoints between whole PASSES: every pass applied
      entirely or not at all, `cancelled` says a later one was not run
- [x] 2.5 The band narrows by what the COMPLETED passes could have moved the
      surface, never by what all of them would have

## 3. `rewrite_region_tallied` — what a rewrite selected

- [x] 3.1 `FieldVolume::RewriteTally { bounds, touched_bricks, changed }`
- [x] 3.2 `rewrite_region_tallied`; `rewrite_region` is it with the report
      dropped, so there is one walk and not two
- [x] 3.3 The bounds and the count are GEOMETRIC — the bricks the region
      selected — and `changed` is the separate value question

## 4. The Smooth transaction

- [x] 4.1 `begin` samples the layer ONCE through `scene::bake_layer`, so the
      sampling is the one consolidation already does and a commit installs
      something a bake could have produced. The fingerprint is stamped here
- [x] 4.2 `update` relaxes the working volume in place and returns
      `SdfSculptDirty`. No compile, no bake, no command, no undo entry
- [x] 4.3 `cancel` is lossless: nothing persistent was ever written, so there
      is nothing to unwind. The working volume is released, because a cancelled
      preview is not a thing a host should still be able to draw
- [x] 4.4 `commit` installs the working volume through
      `replace_layer_with_volume` — no second bake — inside one undo bracket
- [x] 4.5 `commit` REFUSES a layer that changed, was removed or was protected
      since `begin`, changing nothing
- [x] 4.6 `begin` refuses an unknown, non-SDF, edit-list-less or protected
      layer, a cell size of zero, and a field that could not be sampled
      (empty, unbounded, or cancelled through the token)
- [x] 4.7 `preview_volume()` for the host to draw, `live()` false once the
      transaction is dead

## 5. The Move transaction

- [x] 5.1 `begin` prepares the drag ONCE and captures each affected item's id,
      prepared frame and PRE-STROKE deformer chain by value
- [x] 5.2 `begin` builds a private preview `Layer` with its own `SdfContent`,
      so the host compiles, draws and picks it through the paths it has
- [x] 5.3 `update` takes the TOTAL world displacement and rebuilds each preview
      chain from the captured original — never from the last frame
- [x] 5.4 `update` reports a swept dirty bound: the anchor's ball united with
      the ball the drag has reached
- [x] 5.5 `commit` rebuilds the final chains from the same captured state
      rather than trusting the preview, as one `SetDeformersCmd` per item
      inside one undo bracket
- [x] 5.6 A displacement of zero commits nothing; a drag that reaches nothing
      is a valid transaction with no affected items, not a refusal
- [x] 5.7 `begin` refuses a non-positive radius and everything §4.6 refuses
- [x] 5.8 `affected_count()`, `prepare_stats()` and `last_update_visited()` —
      the counters that make "the traversal is paid once" a number

## 6. The complexity policy

- [x] 6.1 `SdfSculptComplexityPolicy`: `min_safe_step_scale`,
      `max_deformer_chain`, `max_item_count`, kept SEPARATE because the report
      keeps them separate. Zero disables a criterion
- [x] 6.2 `over_sculpt_budget(policy, report)` — pure, no document, so a host
      can ask it about a report it obtained itself
- [x] 6.3 `allow_consolidation` is OFF by default: over budget without it is a
      report and changes nothing
- [x] 6.4 An authorised collapse runs inside the gesture's OWN undo bracket, so
      one stroke is one undo
- [x] 6.5 The collapse resamples at the policy's own numbers, falling back to
      the gesture's cell size / band / padding when it states none
- [x] 6.6 A layer that is already a single volume is NOT baked again —
      `consolidation_state` is exactly that question, and Smooth's own commit
      leaves the layer in that state, so without this an over-budget Smooth
      would resample its own samples into a volume of a volume at a worse
      Lipschitz
- [x] 6.7 `SdfSculptBudget` reports the report, `over_budget`, `consolidated`
      and the cost; the installed cost is kept when nothing was consolidated

## 7. Nestable undo grouping

- [x] 7.1 `bool grouping_` becomes `int group_depth_`; at depth 0 it is exactly
      what it was
- [x] 7.2 Nested brackets collapse into the outermost step
- [x] 7.3 An unbalanced `end_group` is ignored rather than corrupting the
      stack, and the next command opens its own step
- [x] 7.4 An outermost bracket that recorded nothing still records nothing,
      including when it held only empty inner brackets

## 8. `replace_layer_with_volume`

- [x] 8.1 The install half of `consolidate_layer`, extracted rather than
      duplicated: `consolidate_layer` is `bake_layer` then this
- [x] 8.2 Everything the collapsed form guarantees, because it is the same
      code: the instance sever, one group whose inverse restores the absorbed
      items with their ids, parameters, colours and deformers, the protected
      refusal, the layer transform left alone, the first colour carried onto
      the volume
- [x] 8.3 It does NOT check that the volume is a plausible bake of this layer.
      It cannot — a volume is a volume — and the caller owns that claim

## 9. `prepare_move` / `resolve_prepared_move`

- [x] 9.1 `PreparedMove` holds the anchor and reach in the item's own frame and
      the terms that turn a world displacement into a local one — kept as the
      terms `move_brush` divides and rotates by, not pre-inverted equivalents
- [x] 9.2 `MovePrepareStats { visited, reached }`
- [x] 9.3 `prepare_move` walks the tree; `resolve_prepared_move` is O(1) with
      no scene access
- [x] 9.4 `move_brush` becomes prepare-then-resolve, so there is one geometry
      and not two
- [x] 9.5 `moved_chain(chain, warp)` for a caller holding the pre-stroke chain
      by value; the node overload is this applied to `node.deformers`

## 10. The source fingerprint

- [x] 10.1 `layer_fingerprint(layer)` — FNV-1a over everything an edit could
      change, stamped at begin and re-checked at commit
- [x] 10.2 The CONTENT, not the `shared_ptr`: an instance layer shares its edit
      list, so an edit through a sibling is an edit here and the address has
      not moved
- [x] 10.3 Floats mixed by their BITS, so a value that prints the same cannot
      fool it — a spurious refusal at worst, never a missed one
- [x] 10.4 Shared immutable payloads (volume samples, gates) folded in by
      identity and size rather than by hashing megabytes
- [x] 10.5 A dangling root hashes as its own state, told apart from the node
      being present

## 11. Build and layering

- [x] 11.1 `src/session/sdf_sculpt.cpp` into `CMakeLists.txt`
- [x] 11.2 `tests/unit/test_sdf_sculpt.cpp` into `tests/CMakeLists.txt`
- [x] 11.3 `session/` depends on `scene`, `brush` and `field`, and nothing
      depends on it — the direction `check_layering.py` enforces

## 12. Tests: the transactions (`tests/unit/test_sdf_sculpt.cpp`)

- [x] 12.1 "Nothing happened" is asserted as SERIALIZED BYTES, not as a pointer
      comparison, and it is the first thing every case checks
- [x] 12.2 A live Smooth changes its preview and not the document: `begin`
      alone edits nothing, an update moves the preview at the seam, and the
      document is byte-identical throughout
- [x] 12.3 A live Smooth pushes no undo step until it commits, and then exactly
      one
- [x] 12.4 The live sequence equals the standalone sequence: the working volume
      is byte-identical to the same dabs through `field::relax`, and so is the
      item the commit installs
- [x] 12.5 One commit is one undo step; undo restores the two balls with their
      parameters and redo restores the volume
- [x] 12.6 The source layer is evaluated ONCE, at begin — the evaluation count
      after begin is the count after the updates and after the commit
- [x] 12.7 A live Move leaves every persistent chain alone: no
      `SetDeformersCmd` reaches the document, the document's items carry no
      deformers, and the preview's carry one each
- [x] 12.8 The Move preview is the TOTAL displacement: a sequence of updates
      evaluates as a single fresh drag of the final displacement, and each item
      carries exactly one warp
- [x] 12.9 Preparation once, then work proportional to what moves: over layers
      of 2 and 5,002 items, `prepare_stats().visited` is 2 and 5,002 while
      `last_update_visited()` is 2 for both
- [x] 12.10 The refusals, and the cancels: a layer that cannot be owned, a
      radius that is not a drag, a drag reaching nothing that commits nothing,
      and a cancelled Move that leaves the document as it was
- [x] 12.11 A whole Move drag is ONE undo step across every affected item, with
      one grab per item rather than one per frame, and undo/redo exact
- [x] 12.12 Separate Move strokes still compose and one drag still does not:
      three strokes leave three grabs and a degraded safe step scale
- [x] 12.13 An unauthorised policy reports over budget and bakes nothing — the
      layer's spheres are still spheres
- [x] 12.14 An authorised policy collapses the layer INSIDE the stroke's own
      undo step: one entry for the whole gesture, the chain gone, and one undo
      back to before the stroke
- [x] 12.15 A stale transaction cannot overwrite a concurrent edit, for Smooth
      and for Move: the commit fails, the external edit survives, and no
      partial step was installed
- [x] 12.16 A masked Smooth freezes exactly what the mask covers, and a
      partially cancelled dab keeps the passes it paid for
- [x] 12.17 A Smooth commit does not re-bake a layer it just sampled: with a
      budget nothing can satisfy, `consolidated` is false and the installed
      volume is byte-identical to the preview
- [x] 12.18 The fingerprint moves for a transform, a parameter, a deformer and
      an edit through a sibling INSTANCE — and is stable when nothing moves
- [x] 12.19 An empty policy authorises nothing and is never over budget

## 13. Tests: the pieces this splits

- [x] 13.1 `test_relax.cpp`: in place equals returning, one pass and a
      sequence; an empty volume; strength zero selects bricks and moves
      nothing; a mask; a cancel before the first pass with the band unmoved,
      and `relax` still returning its input; the band narrowing identically;
      the measured slope never rising over five in-place passes
- [x] 13.2 `test_move_brush.cpp`: prepared-then-resolved is BIT-identical to
      `move_brush` across a blended pair, a rotated item under a transformed
      layer, a per-axis scale, `front_only` with a non-default ease, a group,
      nothing in reach, and a radius that is not a drag; `visited == 2002` for
      the preparation with resolving costing no traversal; the two
      `moved_chain` forms agreeing, coalescing rule included
- [x] 13.3 `test_consolidate.cpp`: installing a volume is byte-identical to
      what `consolidate_layer` installs, is one undo step with an exact
      inverse, refuses locked, ghosted and unknown layers, and severs shared
      instance content with the sharing restored by one undo
- [x] 13.4 `test_commands.cpp`: an inner bracket does not open a second step;
      an unbalanced `end_group` is ignored and the next command opens its own
      step; an empty outer bracket holding an empty inner one records nothing

## 14. The cost claims, and what holds each one

- [x] 14.1 Smooth samples the layer once per GESTURE, not once per dab — held
      by 12.6, as a counter
- [x] 14.2 A Smooth dab costs the bricks its region selects — held by the
      tallied rewrite (3.3) and by 12.16's frozen samples
- [x] 14.3 A Smooth commit installs, and does not bake — held by 12.17's
      byte-identical volume
- [x] 14.4 A Move frame costs the items the drag moves, whatever else the
      document holds — held by 12.9 and 13.2 as counters, not clocks, so it
      holds on a loaded CI machine as firmly as on an idle one
- [x] 14.5 A Move drag writes the persistent document ONCE — held by 12.7 and
      12.11
- [x] 14.6 MEASURED: `BM_SdfSmoothTransactionBegin` covers `begin()`, which is the
      whole-layer bake this change deliberately pays at pointer-down. It is the
      number that decides whether local checkpoints are worth building, so it
      is the first thing §17.3 adds

## 15. Proving the tests

- [x] 15.1 REVERT A: `relax_in_place` copies and swaps rather than smoothing in
      place. The byte-identity cases still pass and the cancellation cases
      fail — which is the point: the two forms differ ONLY in what a cancel
      means
- [x] 15.2 REVERT B: `SdfSmoothTransaction::commit` re-bakes the layer instead
      of installing the working volume. 12.4 and 12.17 fail on bytes; the
      shape-level cases pass, which is why they are not the ones asserted
- [x] 15.3 REVERT C: `SdfMoveTransaction::update` composes each displacement
      onto the previous preview chain instead of resolving the total from the
      captured original. 12.8 fails
- [x] 15.4 REVERT D: `begin_group` pushes an entry at every depth. 12.14 and
      13.4 fail — one stroke becomes two undos
- [x] 15.5 REVERT E: `commit` skips the fingerprint re-check. 12.15 fails on
      both transactions, and the external edit is silently destroyed
- [x] 15.6 REVERT F: `layer_fingerprint` folds in `layer.sdf` by pointer rather
      than walking the content. 12.18's instance case fails — the one that
      matters, because the pointer cannot move when the edit comes through a
      sibling
- [x] 15.7 REVERT G: `settle_budget` drops the `consolidation_state` guard.
      12.17 fails: a volume of a volume, at a worse Lipschitz, for no change in
      what the layer costs
- [x] 15.8 REVERT H: `prepare_move` recomputes the reach per resolve. 12.9 and
      13.2 fail on the visit counters
- [x] 15.9 Each revert COMPILES under `-Werror` before its result is believed

## 16. Gates

- [x] 16.1 `cmake --build build/cpu-only`, `ctest --preset cpu-only` — green;
      `pyclay_pytest` fails identically on `main` (an import quirk of the local
      anaconda interpreter) and is excluded rather than claimed
- [x] 16.2 `check_layering.py` (the table gains `session -> brush`, documented
      in place: brush's own set is already a subset of session's and brush
      knows nothing about session, so it adds no transitive edge and no cycle),
      `check_c_abi.py` hygiene + the ctypes FFI exercise, `check_binding_parity.py`,
      `check_doc_latency.py`, `check_kernel_dialect.py`, `check_licenses.py`,
      `check_swift_package.py`, and `release_check.py`'s version gate
- [x] 16.3 `openspec validate add-sdf-sculpt-transaction --strict`
- [x] 16.4 Version bump 0.58.0 -> 0.59.0 in CMakeLists.txt, clay.h
      (`CLAY_ABI_MINOR`) and pyproject.toml. A MINOR bump: the ABI only gains
      entry points, and every existing declaration is untouched

## 17. Landed after the first pass

- [x] 17.1 THE C SURFACE. `clay_sdf_smooth_*` and `clay_sdf_move_*`: two opaque
      handles with begin/update/commit/cancel, a `destroy` that IS a cancel, and
      three versioned descriptors (`clay_sculpt_policy`, `clay_sculpt_dirty`,
      `clay_sculpt_budget`). The preview half went the way the note below asked
      for rather than the way that was easiest: Smooth hands back a `clay_item*`
      carrying a COPY of the working volume, so nothing the host compiles can be
      mutated under it, and the copy is documented as the cost it is; Move hands
      back the affected node ids by the size-query pattern plus the resolved
      grab as the parameters `clay_item_add_deformer(CLAY_DEFORM_GRAB, ...)`
      already takes, so a host draws the preview through machinery it has and no
      new struct crosses the boundary. Covered by `tests/unit/test_c_sdf_sculpt.cpp`,
      which asserts the mid-gesture `clay_document_save_memory` is byte-identical
- [x] 17.2 THE DOCS. `docs/07-brushes-and-features.md` §3.1/§3.2 (what a
      transaction guarantees, why the edit-list brushes do not have one, TOTAL
      displacement, the stale-source refusal, and the policy's "the engine never
      decides to bake"), `docs/05-claycore-library.md` §3 (the module map gains
      `session/`) and §11 (the C surface), and `docs/09-brush-latency-and-coverage.md`
      with the measured before/after
- [x] 17.3 THE BENCHMARKS. In `benchmarks/bench_main.cpp`, with gates in
      `tools/check_bench.py`: the Smooth pair (standalone bake-and-relax against
      a transaction dab), `begin` on its own axis so §14.6's unmeasured cost is
      measured, the whole-gesture pair at 100 and 1000 dabs, the Move pair, and
      the two PARAMETERISED scaling families that hold the affected set constant
      while the unrelated item count grows — which is the claim a ratio gate can
      hold and a wall clock cannot
- [ ] 17.4 LOCAL CHECKPOINTS for Smooth, so `begin()` stops being O(model).
      Deliberately after the benchmark, not before it — see `design.md` on why
      a local patch was not the P0. The benchmark that decides it now exists

- [x] 17.5 WHAT A COMMIT COSTS THE MARCHER, said and gated (issue #379). The
      layer that comes back is one sampled volume, and `kernel::cfi_volume`
      declares `sqrt(3) * max(sample_lipschitz, 1)` for one — so a layer's
      safe step scale falls from 1.0 to 0.577 and the sphere trace takes 7.1
      steps a ray parametric, 22.9 consolidated at the SAME shape, and 33.8
      after a committed Smooth — most of it the lattice, the rest the relax. Stated on `clay_sdf_smooth_commit` and in
      `docs/05-claycore-library.md` beside the CPU-only bake statement, with the
      consequence a host cares about: a renderer with a FIXED step budget draws
      a committed Smooth worse than the parametric layer it replaced, and two
      renderers with different budgets disagree about the same document. Pinned
      by `tests/unit/test_sdf_sculpt.cpp` ("a commit lands at the ceiling a
      sampled volume declares"), which also records that a
      `min_safe_step_scale` above `1/sqrt(3)` is unsatisfiable.

      AND THE COVERAGE GAP UNDER IT. The backend raycast comparison ran over one
      scene — a sphere and a box, both 1-Lipschitz, ~9 steps a ray — so no
      backend was ever compared on a march through a sampled volume, here or in
      the Swift device suite, which has no raycast at all. `test_parity.cpp` now
      raycasts a layer smoothed and COMMITTED through the real transaction as
      well, with teeth on the step scale and on the step count so the second
      scene cannot degrade into a second easy one. Verified against a real
      device backend locally (Vulkan/lavapipe), not only the CPU

- [x] 17.6 THE MARCH, IN THE HOST PARITY FIXTURE (issue #379 follow-up). The
      fixture asked only what a host EVALUATES, so a preview that got every
      distance right and then traced it wrongly passed — while `docs/06` has
      said "step by `safe_step_scale`, never by 1" since the loft and sweep
      cases landed, with nothing enforcing it. Schema 2 adds a `march` block
      and per-case `rays`: rays that hit, and where the CPU reference lands.
      Purely additive, so a schema-1 consumer reads a schema-2 file unchanged.

      The tolerances are LOPSIDED and that is the design: `hit_t_late_abs` 1e-3
      against `hit_t_early_abs` 1e-2, because the step scale sits in the
      acceptance test as well as the step length, so a host that marches more
      conservatively than asked lands EARLY and is safe, while overstepping
      lands LATE or misses. Every exported ray is filtered to one a
      differently-written marcher also passes — over-relaxation off, a finer
      and a coarser eps, half the budget, a quarter of the step scale — so a
      case may carry few rays or none rather than shipping an expectation a
      correct consumer cannot meet.

      Gated three ways in `test_parity_fixture.cpp`: the rays exist and reach
      the bound cases, every one survives those five variant marchers, and a
      step-by-1 marcher is REJECTED by at least two cases. Also compared on
      every registered backend's own `raycast`, beside the point comparison.

      AND WHAT IT DOES NOT CATCH, measured and written down: a sampled VOLUME
      does not discriminate a step-by-1 host at any cell size tried. `sqrt(3)`
      is what `cfi_volume` declares for a lattice, but a redistanced volume's
      realised gradient sits near 1 and a ray does not ride the cell diagonal.
      Steep deformer fields are what catch it — deformer_chain 3 of 4 rays,
      relief_build_up 13 of 96, deformer_noise 4 of 86. Recorded because
      reasoning from the declared bound to a visual consequence is the step
      that made #379 look explained when it was not.
