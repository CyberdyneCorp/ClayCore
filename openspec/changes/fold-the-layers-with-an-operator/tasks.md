## 0. Decide first

- [x] 0.1 SETTLED (design.md "Decision — task 0.1"): SPLIT AT THE LAST HARD
      BOUNDARY, which in this tree is the only boundary — the seam is always the
      last visible SDF layer, so the split stays available whenever THAT layer's
      composition is a hard Add, whatever the layers beneath it do, and
      `fold_layers_below` is left unchanged.
      PRICED, which the decision said it would be and which was missing until
      the record stage: `BM_BrickRefillLayersUnion` and
      `BM_BrickRefillLayersComposed` are the same two-layer 5,000-item document
      and the same stroke, differing only in the top layer's composition —
      0.009 ms against 7.96 ms, which is the pre-#348 walk restored for that one
      shape. The claim is the REFUSAL and a refusal is invisible in the values,
      so it is gated as a count: `resumed_frac` 0.0 on the composed arm (ceiling
      0.0) and `refilled_frac` 0.0 on the union arm (ceiling 0.05), with a
      MAX_RATIO pair naming both rows so they cannot pass by not running —
      MAX_COUNTER skips an absent benchmark. Proved by deleting the three
      `layer_join_is_hard_union` refusals: the composed arm resumes everything
      (`resumed_frac=1.000 above ceiling 0.000`) and the ratio row fails at
      0.92x

## 1. The model

- [x] 1.1 `LayerComposition` on an SDF layer, using the EXISTING item enums
- [x] 1.2 Accessors, with validation: enum range, finite floats, SDF layers only
- [x] 1.3 A non-SDF layer REFUSES rather than storing dead state
- [x] 1.4 Writing at a minor below 18 REFUSES a document carrying any non-default
      composition (design.md §7), and is allowed and byte-identical to what 17
      meant for a document where every layer unions. Not a silent degrade: a
      subtractive layer written as a union is a different model
- [x] 1.5 A query a host can call BEFORE it saves: can this document be written
      at minor N without losing authored intent? Across the C ABI, with the
      refusal itself returning `CLAY_ERROR_UNSUPPORTED`

## 2. The fold

- [x] 2.1 `run()` folds instead of unioning — and so does `run_part()`, which is
      the same loop: a PART whose layers still hard-unioned internally would not
      be the accumulator `run()` holds at that boundary, which is the one thing
      the split depends on. Both go through one `Compiler::fold_layer`. A layer
      whose chain produced nothing (empty, all-hidden, or culled out of a brick)
      is skipped where its operator reads an absent operand as no change and
      folded against the far field where it does not — an intersecting layer
      that is skipped leaves material the whole-document tape removes, which is
      the sharpest silent case in the whole change and was not in the audit
- [x] 2.2 The first visible layer initialises and its op is NOT applied — and
      WHICH layer that is comes from the document's visible SDF layer LIST, not
      from the compile's accumulator flag. Reading it off `have_acc` was a
      silent wrong field: a brick whose cull drops every item beneath a composed
      layer arrives with `have_acc` false, and the layer was then promoted to
      the initialiser FOR THAT BRICK — an intersecting cutter returning a solid
      sphere where the document has nothing (512/512 samples, no error, both
      through `compile_document(&cull)` and through
      `clay_brick_cache_eval_requests`). `Compiler::compile_and_fold_layer`
      takes `first` from the walk's own layer selection, which makes a PART a
      document in its own right as well (`Only` is the layer alone, `Except` is
      the document without it). The item rule's OTHER half DOES lift, but only
      for a layer that is not the first: with the accumulator absent a carving
      operator drops the layer, a material-creating one (Shell, Replace) folds
      against an explicit empty, and a union takes the layer as it is — verbatim
      what `compile_list` and `compile_group` do with an item that opens a
      chain, which is what keeps a subtracting LAYER and a subtracting ITEM the
      same document. `clay_test::ref_eval_document` needed the same correction:
      an independent evaluator that repeats the defect agrees with it
- [x] 2.3 One low-level combine emitter shared with the item path; the kernel
      math stays single-source — `Compiler::emit_chain_combine`, which was
      already duplicated byte for byte between `compile_group`'s tail and
      `resume`'s stack unwind and would have become a third copy here
- [x] 2.4 A layer's symmetry resolves before it combines, once — structurally
      already true (mirror and radial copies are emitted inside `emit_item` and
      folded into the layer's own chain long before it folds), so this cost no
      code and is pinned by a test that breaks if the layer combine is ever
      hoisted earlier

## 3. Correctness

- [ ] 3.1 Bounds PER OPERATOR, from the item-level logic — subtract is bounded by
      its left operand, intersect by the intersection. HALF DONE, and the half
      that was done is the half that can lose surface. `fold_layer_bounds` now
      dilates the layer's extent by the fold's OWN support, through
      `scene::chain_blend_support` — one expression, which `group_blend_support`
      and the new `layer_blend_support` both call and which the item path
      already applies inside `geometry_bound`. That closes the case a bound can
      be too SMALL: a smooth or extended fold bulges past the union of both
      operands, and until this the layer fold reported the plain union (proved
      by reverting it — 11,618 lattice samples then carry material outside
      tape.bounds). The NARROWING rows of design.md §3 are deliberately NOT
      taken and the reason is written beside the code: the item path unions for
      every operator too, and the parity gate in 3.3 is that a subtracting LAYER
      and a subtracting ITEM are the same document — narrowing one side alone
      breaks it, and narrowing both changes the meshing region of every document
      that already carries a subtract or a paint AND has to be threaded through
      compile_group's rollback and every resumable entry point that copies a
      prefix's bounds. Being wider than necessary costs a larger march; being
      narrower than the surface costs the surface. It belongs in its own change,
      with its own measurement
- [x] 3.2 Exactness and Lipschitz fold as the item combine folds them — the
      shared `emit_chain_combine` was folding EVERY extended mode through
      `cfi_extended_blend`, which against a field info and itself is
      `{false, L}`: it charged a Relief nothing at all, where the item path has
      always spelled it `cfi_relief`, the term's own gradient. Reverted, the
      layer form reports L = 1 against 2.8 and `check_conservative_steps` walks
      through the surface. The same defect lived one level down in
      `compile_group`, which shares the emitter and had no test at all; it now
      has one
- [x] 3.3 THE PARITY FIXTURE: two layers vs one layer of two items, over many
      points, in distance, colour, bounds and safe step — `test_layer_parity.cpp`,
      in both forms: one item per layer against one layer of two items (the
      spec's own scenario, where the two tapes are the same tape byte for byte)
      and several items per layer against one layer holding a GROUP, which is
      the only correct one-layer equivalent of a composed layer of several. Union,
      smooth union, chamfered union, subtract, smooth subtract, intersect, smooth
      intersect, paint, groove, shell and incise, each against the hard-union arm
      so a fold that ignored the composition could not pass

## 4. The six sites that assume a hard union

- [x] 4.1 `compile_document_resumable`'s trailing union — the fold stage landed
      the EMISSION (`Compiler::resume` re-emits the ACTIVE LAYER's composition,
      derived from the `const Layer&` it already takes rather than carried on
      the checkpoint, so a hand-built checkpoint cannot assert an operator that
      has stopped being true). This stage landed the rest: the header
      restatement, and the composed subcases in `test_tape_prefix_reuse.cpp`
      — a composed layer BENEATH the seam keeps the reuse and stays
      bit-identical, a composed SEAM refuses it (see 4.4). The hand-built
      checkpoints in `bindings/c/clay_c.cpp` needed no change: their
      `doc_have_acc = false` statement says "the suffix emits no fold, the
      refill rejoins it", and the refusal in `plan_resume`/`plan_frontier` is
      what makes that statement true rather than assumed
- [x] 4.2 `compile_document_part` — REPAIRED, not refused. Both halves stay
      correct compiles and only the JOIN changes: it is the active layer's own
      composition, and the split is bit-identical because it is taken at a
      layer boundary, where `below` is exactly the accumulator the
      whole-document walk holds. `scene::layer_join_composition` is that
      combine, `layer_join_is_hard_union` is the test a caller that cannot
      apply an arbitrary one takes, and the header now says both. Tested by
      rejoining the two halves sample by sample through the kernel's own
      combine and requiring the whole-document field back, over union, subtract,
      intersect, smooth and extended folds, with the teeth that rejoining with
      an Add instead is a different field
- [x] 4.3 `compile_document_except` — NOT repairable and not repaired. The SUM
      promise is deleted from the header (with A, B(Subtract), C and excluded =
      B, no combine of A+C and B is (A−B)+C) and the compile keeps its own
      meaning. The four callers whose contract IS the min composition refuse
      with `CLAY_ERROR_INVALID_ARGUMENT` naming the layer:
      `compile_document_without` (both eval forms),
      `clay_brick_cache_eval_requests_excluding`, and pyclay's `eval_excluding`
      / `gradients_excluding`. `test_c_eval_excluding.cpp` is UPDATED rather
      than weakened — the zero-differing-samples assertion is kept, with a
      comment saying it now holds only while every layer unions, and a new case
      asserts the refusal
- [x] 4.4 `compile_document_append`'s `info` / `lipschitz_bounds_gradient` /
      `bounds` carry-over — REFUSES when the seam is not a hard Add, so the
      comment's justification is true wherever the function proceeds. Measured
      with the refusal removed: a smooth-union seam reuses a prefix whose
      `bounds.min.x` is −1.5 where the whole-document compile says −3.0, because
      the fold's ring follows the layer's extent and the appended item enlarges
      it. A box that small is a dropped brick and a lost ray hit — missing
      surface, not an error
- [x] 4.5 The brick refill's multi-layer split — `plan_resume`, `plan_frontier`
      and `eval_requests_impl` all refuse it through one predicate
      (`layer_join_is_hard_union`), and the refused full path stores NO seed,
      following `resume_batch_into_host`'s "nothing rather than something
      mislabelled". `fold_layers_below` is unchanged byte for byte and its
      comment gains the precondition and the argument for why it is unreachable
      otherwise — an ARGUMENT that was itself backwards when first written (it
      credited the revision bump, which a region invalidation carries a
      surviving seed straight past) and is corrected in 7.8. `has_below` keeps meaning "more than one visible SDF layer" for
      the three callers that probe it as topology; the refusal lives in `usable`
      alone
- [x] 4.6 Each DECIDED and tested. Every refusal is asserted as a COUNT
      (`clay_document_resume_stats.resumed_bricks`, never a clock) because a
      refused resume is bit-identical to the walk it falls back to; and every
      field claim is checked against something that does not share the code
      under test — a one-layer document expressing the same shape as items,
      which takes no split at all. NOTE the one honest gap: the three refusals
      in 4.5 are individually invisible (removing the store's leaves the plans
      refusing, removing the plans' leaves no seed to refuse), so the count test
      holds them as a conjunction. Reverting `eval_requests_impl`'s alone is
      still caught, by the field

## 5. Invalidation

- [x] 5.1 Composition joins the key of the tape, the cull index, the prefix
      cache and the brick seed store. The digest half landed in the model stage
      (`digest::mix_layer_head`, so `layer_prefix_fingerprint` and
      `layer_fingerprint` both move); the tape and the cull index are keyed on
      `revision` and follow `apply_edit` for free; the brick seed store is
      deliberately NOT keyed on the composition (the decision's `ResumeKey`
      note) and is protected by 4.5's split refusal instead. What this stage
      added is THE CULL PAD, which had no inter-layer term at all. The term is
      NOT a per-layer constant: a fold drags the items of every layer BENEATH
      it, and a stack of folds composes, so what the pad carries is
      `folds_from_layer_support` — the SUM of the folds above a layer, charged
      to that layer — resolved in `scene::document_cull_pad` and in
      `CullIndex::refresh_pad`, which are the two readers a compile picks
      between and which a test now holds equal. Without any term a per-brick
      compile under a smooth fold drops an item the whole-document compile
      keeps: deleting it leaves `differing()` at 21 of the 84 values sampled in
      the single-fold fixture's region (the distance at every one of its 21
      points), inside the band where nothing is looking. With the term MAXED
      rather than summed — the first form of this fix — a document of N folds
      was padded for one of them: 0.0180 of band drift at two composed folds and
      0.0268 at three, measured by the sweep in 5.2
- [x] 5.2 Conservative first — and the cull pad's fold term was NOT one of the
      places where that was true. It took the fold's full support per layer and
      then MAXED over the layers, which is not a conservative narrowing of the
      sum but a different and smaller number: a chain of N folds drags further
      than one of them does, and the max counted one. Corrected to the sum
      (`folds_from_layer_support`, which the change already spelled 250 lines
      away for the dirty-region half and which says why they compose), and the
      only conservative choice left in the pad is the one that direction: the
      fold's FULL support rather than a chain envelope over the layer count.
      Measured by a 240-region sweep over four spheres 0.62 apart with the top N
      folds composed — worst band drift 0.0180 at two folds (k = 0.3) and 0.0268
      at three (k = 0.45) under the max, 0 under the sum, and 0 in both for the
      all-hard baseline; dilating each region by a further 2k took the two-fold
      row to 0, which is how the pad was told apart from the fold. A wider pad
      is a longer tape; a narrower one is wrong geometry, and the two are not
      symmetric — which is why the FIRST visible layer's own composition, never
      applied, is not a term either. Two places where this stage did choose the
      wide answer and said what it costs beside the code:
      an intersecting layer's dirty box is the union of the
      layers beneath, which is the box the host measured at 45.5 ms and 7.5 s
      and which wants a REFILL-REGION narrowing, not a bounds one; and the
      below-extent walk is not memoized, because a cache whose only observable
      is that it stopped firing needs the `walks()`/`keeps()` counters
      `LayerExtentCache` carries and a measurement to justify it
- [x] 5.3 Dirty influence for a moved, re-blended or hidden cutter.
      `scene::layer_influence_bound_in_document` — which holds the `Document`,
      so it can see the stack, and which is why `layer_influence_bound` is not
      widened in place — dilates by the fold's own support and, for an Intersect
      and nothing else (`op_is_local`), unions the extent of the visible SDF
      layers BENEATH. `layer_command_bound` is that function plus the
      first-visible flip, and the two host-facing layer routes are that function
      alone (7.4). That covers `SetLayerVisibleCmd`, a reorder
      (a Remove+Add pair) and the composition command itself. An edit made
      INSIDE a lower layer is deliberately not widened: a combine is pointwise,
      so it changes the folded result exactly where it changed the accumulator.
      Also here, from design.md §11: `layer_scales_cleanly` now reads the
      layer's composition, so a soft fold classifies GENERAL and the placement
      gesture stops skipping invalidation on a similarity that is not one — with
      the trade (an absolute radius, or an edit per scale) stated in the header
      beside the composition setter, where a host will actually meet it. That
      first form read only the blend PROFILE and so still classified an extended
      fold's radius as clean, and its regression sat at the predicate rather
      than at the ABI the requirement names; both are closed in 7.8
- [x] 5.4 THE TWO REACHES THAT WERE MISSING, both silent and both now measured
      by comparing every changed sample against the box the command reports.
      (a) EVERY FOLD ABOVE, not only the layer's own: an edit is carried up the
      stack through each fold it passes, so `node_command_bound` and
      `layer_command_bound` both dilate by `folds_from_layer_support` — the sum
      of the layer's own fold support and every fold above it, which is
      `node_reach_bound`'s per-group dilation one level up, where
      `node_reach_bound` stops because it holds a Layer and not a Document.
      5.3's "an edit inside a lower layer is deliberately not widened" was right
      only for a HARD combine; measured with it reverted, an ordinary dab under
      one soft fold leaves band-relevant samples changed outside the reported
      box. (b) THE FIRST-VISIBLE FLIP: adding, removing, hiding or showing the
      bottom-most visible SDF layer — and the Remove+Add pair a reorder is —
      turns the layer above it from folded into initialising, so a subtractive
      cutter comes back as the base shape over its OWN whole extent.
      `first_visible_flip_bound` covers it, for a composed next layer only,
      since promoting a hard union differs only where the edited layer had
      material. Measured with it reverted: a refill answers three whole bricks
      from a `below` half computed for a document that no longer exists

## 6. Gates

- [x] 6.1b Hide/reorder the BOTTOM layer, which the gate below does not reach:
      hiding the CUTTER and hiding the layer BENEATH it change the field by
      different mechanisms, and only the second one moves the first-visible
      rule. The fixture had to be measured rather than reasoned about twice
      over — the top layer must UNION (a composed seam stores no seed) and must
      be WIDE enough to reach the outer bricks, because the promotion only ever
      turns empty space into material and a brick that held nothing before the
      edit is not answered from a lattice seed at all. With the widening
      reverted, three bricks come back resumed and unchanged where a document
      built that way from scratch moves all three
- [x] 6.1 Hide/show a subtractive layer restores exact geometry — and the half
      the geometry cannot see. Hiding goes through a dirty REGION, and a brick
      outside it keeps what it had and is re-stamped to the new revision, so a
      fresh compile after the edit proves nothing about it. The gate is
      therefore a REFILL against a document built the same way from scratch,
      with the cutter INTERSECTING and its bricks outside its own box — and it
      needs a third, plain unioning layer above, because a composed SEAM stores
      no seed at all (4.5) and a decorative layer that reaches no brick leaves
      `resumed_bricks` at zero, either of which makes the gate measure nothing.
      Reverting 5.3's widening leaves 512 samples — one whole brick — stale
- [x] 6.2 Order matters: A−B+C differs from A+C−B, stably across a reload — and
      the same invalidation half, because a reorder is a Remove+Add pair each
      bounded by the MOVED layer's own extent. Held three ways: the two stacks
      differ, each reloads bit-identically, and moving the layer in an EXISTING
      document is the other stack exactly. The refill arm fails by 512 samples
      with the widening reverted
- [x] 6.3 Old documents load unioning and render bit-identically — bytes written
      at minor 17, which is what a pre-feature build wrote, read back with every
      composition at its default and compiling to the SAME TAPE: instrs, params,
      `is_exact`, Lipschitz and safe step, not merely the same answers at the
      points sampled
- [x] 6.4 Undo/redo through the existing layer-property history — the value and
      the byte-level document restore landed in the model stage; what this stage
      added is that the undone document COMPILES back, now that the compiler
      reads the value, at both the engine level and through
      `clay_document_undo`
- [x] 6.5 A converted mesh-to-SDF layer works as a cutter — `mesh::to_field` in
      its OWN layer, set to subtract, agreeing sample for sample with the same
      volume as a subtracting item in one layer, and hiding it giving the
      uncarved form back exactly. This is the organisation the change is for:
      an imported mesh used to have to live in the layer it was cutting.
      AND AS THE BASE (design.md §9's second gate, added by the record stage):
      the same volume UNDERNEATH a field cutter, which is the other side the
      host's `boolean_operands` puts every representation on. Hard subtract and
      SMOOTH subtract, each against the one-layer item form, plus the teeth that
      the cut is real and that the converted layer's own composition is not
      applied when it is the first visible one. The fold turns out to treat base
      and cutter symmetrically, so the gate is the redundant one §9 allowed for
      — but it is not vacuous: dropping the fold's blend from
      `emit_chain_combine` moves 592 of its samples
- [x] 6.6 Benchmarks at 10 / 100 / 1000 layers: a layer op costs about what the
      equivalent item combine costs, and there is no second evaluator.
      `BM_LayerFoldStack{10,100,1000}` against `BM_ItemFoldStack{10,100,1000}` —
      the same 2,000 dabs folded in N chunks with a Subtract, as separate LAYERS
      and as GROUPS in one layer, with the item count held CONSTANT so the row
      measures the fold and not the geometry. 0.376 ms against 0.488 ms at 1,000
      folds, 1.02x from 10 layers to 1,000. The "no second evaluator" half is a
      COUNT and is gated as one: 3,999 instructions on both arms, ceiling 4,200,
      where a second fold per layer reads 4,998
- [x] 6.7 C ABI setter AND getter, pyclay, numbered example, version lines. The
      C pair and the three version lines (0.86.0) landed in the model stage;
      this stage added the pyclay mirror
      (`Document.set_layer_composition` / `.layer_composition`, partial update
      in, whole value out, the blend's SUBCLASS carrying the profile so what
      comes out goes back in), the Swift smoke block, and
      `examples/75_layer_booleans.py`, registered in `EXAMPLES`. Parity verified
      with `--pyclay ... --require-import`, which prints "imported <path>" — the
      bare invocation compares the parsed source against itself and cannot fail

## 7. From the review, and from the host reading the fix

- [x] 7.1 First-ness is a TYPE, not a second bool (design.md §13a).
      `FirstVisibleLayer` in tape_build.cpp's anonymous namespace, the local at
      both call sites is that type, and a transposition is a compile error —
      proved by swapping the arguments at one site and reading
      `error: cannot convert 'bool' to 'FirstVisibleLayer'`, then reverting.
      AND THE SIBLING PAIR, which §13a's closing question asks about and which
      the first pass left as two adjacent bools: `fold_layer(layer, layer_val,
      have_acc)` now takes `LayerLeftValue`, typed at the point the value is
      PRODUCED rather than wrapped at the call, so a transposition has no brace
      to move with it. Proved the same way: `fold_layer(layer, acc, layer_val)`
      is `error: cannot convert 'const bool' to 'LayerLeftValue'`, then
      reverted. The two remaining bools in that path are named for what they
      are: `first_composed_fold_layer`'s document-derived flag was called
      `have_acc`, which is the per-compile cull-dependent quantity it is not,
      and is now `passed_a_layer`
- [x] 7.2 THE SWEEP design.md §13 requires, RUN — and it is what found 7.3.
      Retitled from "the sweep itself is the reviewers' second pass", which
      delegated the requirement rather than meeting it and which the record was
      wrong to leave standing: the sweep was performed, over every place the
      fold path reads state a cull region can change, and it returned a fourth
      predicate nobody had written down — the cull pad, which answered what one
      LAYER'S CHAIN needs where the question is what the DOCUMENT needs. That is
      7.3, and it was a live defect rather than a hypothetical. Five sites are
      now closed: the three §13 tabulates, the pad, and the host-facing bounds
      of 7.4, which is the same question asked from outside. What remains open
      is not a sweep but the reviews: §13's own closing note says a fourth of
      anything needs somebody looking for the same thing a third time, and each
      review round since has found one
- [x] 7.3 THE FOURTH cull-observable predicate: the pad answered "what does ONE
      LAYER'S CHAIN need" where the question is "what does the DOCUMENT need,
      fold included" (design.md §10a). The fold term rode the layer that OWNED
      the fold and both readers max over layers, so a document of N composed
      folds was padded for one. Now `folds_from_layer_support` — the SUM of the
      folds above a layer, charged to that layer, moved into `scene/bounds` as
      the one definition the dirty-region half already used — plus
      `scene::document_cull_pad`, with `CullIndex::refresh_pad` the same
      expression over cached terms and a test holding the two equal.
      Regression: a 240-region sweep at one, two and three composed folds, band-
      clamped identity against the whole-document compile; proved by reverting
      the sum, which reports 0.0180 / 0.0229 / 0.0268 of drift and a pad of 1.2
      where three folds need 3.6, and by reverting the first-visible exclusion,
      which charges 1.6 to a layer whose composition is never applied
- [x] 7.4 THE SECOND REVIEW'S BLOCKERS 2, 3 AND 4: the fold widening had landed
      on the internal command path only, so every HOST-FACING route still
      reported the un-dilated box (design.md §13b, §13c). Closed by there being
      ONE function rather than four agreeing ones: `scene::layer_reach_in_
      document(doc, layer, box)` carries a box from a LAYER's field to the
      DOCUMENT's, `node_command_bound` IS `node_influence_bound_in_document`,
      `layer_command_bound` is `layer_influence_bound_in_document` plus the
      first-visible flip, and the three gesture reaches take the same term where
      they state their own region -- including `clay_layer_move_surface`, which
      design.md §13c identifies as the one of the three a real host drives, and
      on a drag, where too small a region tears the surface behind the pointer. `layer_influence_bound` and `node_reach_bound`
      keep a Layer and now say in their comments why that means they cannot
      answer the question and what to call instead.
      Regression, all seven entry points, each asserting that the box the host
      is handed CONTAINS every band-clamped point the edit changed — and each
      proved by a TARGETED revert:
        * `clay_layer_node_influence_bound` and `clay_brick_cache_mark_dirty_
          nodes`: 1,660 and 536 changed samples outside the box, worst 0.0131
          and 0.0064, on reverting the dilation in
          `node_influence_bound_in_document` alone
        * `clay_layer_influence_bound`: 700 outside, worst 0.0079, on reverting
          that binding alone
        * `clay_brick_cache_mark_dirty_layer`: 536 outside, worst 0.0064, on
          reverting that binding alone
        * the three gestures: a seed placed in the shell the fold adds survives
          the gesture instead of being dropped — `2 == 1` — on reverting the
          `apply_surface_gesture` dilation (the drag and the magnify) and the
          `clay_layer_place_stamps` one (the stroke), separately
      Also: `test_c_undo_bound.cpp`'s blended-group case UPDATED, not weakened.
      It asserted the undo bound was strictly wider than the query, which
      encoded the two disagreeing; the requirement is now asserted against the
      child's own geometry and a new subcase holds them equal. Classified under
      design.md §13d: that file is DOCUMENTATION of the new behaviour and not
      evidence for it — reverting the fix fails only the subcase this stage
      added, because its older lines assert the undo bound, which nothing
      narrowed. The evidence is `test_layer_fold_sites.cpp`, which was only
      appended to.
      NOT reproduced as reported: blocker 3's "every dab left stale bricks". The
      reaches genuinely carried no fold term, but their only consumer dilates
      each seed by `band + pad` and `pad` is `document_cull_pad`, which is
      >= the fold sum by construction — measured at 0 stale samples over 504
      bricks either way. The reach is fixed because the coverage belongs to the
      cull pad and not to the reach; the tests assert the invalidation, which is
      what the contract is about

- [x] 7.5 THE THIRD REVIEW'S BLOCKER 5: design.md §12's deliverable was reported
      as done and did not exist — `clay_brick_cache_eval_requests_below` was in
      no header and no source file, only in design.md. Built now, as the same
      shape as its two siblings (`ChunkHalf::Below`, which the resume split
      already used and which no C entry point reached). Its refusal is the
      NARROW one §12 asks for: only when the named layer is not the last visible
      SDF layer, because the layers beneath may compose however they like —
      `compile_document_part` folds them with their own compositions. Hidden,
      mesh and voxel layers above do not block it, which matters because the
      artist sees those rows. §12a's id is an out-parameter carrying the LOWEST
      visible SDF layer above the named one, so a host says "hide or move that
      subtool" instead of "not available here". §12 item 2 is in the
      `_excluding` header: what to use instead, and that excluding from the
      MIDDLE of a fold has no repair. Regression, §12 item 3: below folded with
      the top layer's own composition equals the whole-document refill, over six
      compositions on a document whose LOWER layers compose (a hard subtract and
      a smooth add), bit-identical in distance AND colour, with the teeth that
      the below half alone differs from the whole and that folding with a min
      instead is a different field. Proved by reverting the half to
      `ChunkHalf::Whole` (it compiles): 3,667 of 4,096 distances and every
      colour differ on the composed arms. NOTE for a later reader: `Except` and
      `Below` are the SAME compile wherever this call is legal — with nothing
      above the named layer the two layer sets are identical — so the choice of
      half is unobservable on the accepted domain and no test can separate them;
      what the tests hold is the refusal that keeps the domain that narrow
- [x] 7.6 §12b, the refusal rule made executable rather than agreed: one test
      case walking every refusal in this change that has an id to give — the
      composition setter on a non-SDF layer, `clay_document_writable_at_minor`
      on a composed document, and `clay_brick_cache_eval_requests_below` on a
      layer that is not the topmost visible SDF one — each asserted to return
      its error code AND a non-zero id naming the layer actually responsible. The setter had no id at all (its
      message said only "only an SDF layer carries a composition") and has no
      out-parameter to grow, so its channel is the message, which now names the
      layer and which the test PARSES rather than eyeballs. Proved by three
      targeted reverts, each of which compiles: dropping the `*out_blocking_layer
      = above` line leaves `blocking == 0` against the layer above; removing the
      refusal entirely returns CLAY_OK where the test wants INVALID_ARGUMENT;
      restoring the setter's old message leaves the parsed id at 0. The
      `clay_document_writable_at_minor` arm is DOCUMENTATION and not evidence
      (design.md §13d): it already returned its id, so no revert of this stage
      moves it
- [x] 7.7 §9's documentation duty, which was unmet in bindings/, include/, src/
      and docs/: the absent-operand divergence is now stated beside the
      composition setter in clay.h — that a DOCUMENT-empty operand is FOLDED
      (an empty intersecting layer blanks the field) where a host's resolved
      boolean skips it, that the engine cannot follow the host's rule because
      "this layer produced no value" is also true of a layer wholly CULLED out
      of the region being compiled, and that a host wanting the two routes to
      agree filters empty operands itself. The behaviour itself was already
      decided and tested ("an intersecting layer with nothing in it empties the
      document", test_layer_fold.cpp), which is what made this a documentation
      duty rather than a code change

- [x] 7.8 THE FOURTH REVIEW'S RECORD STAGE, which is the two majors, the minors
      and the accuracy of this file (design.md §13f).
      CODE:
        * `fold_layer(layer, layer_val, have_acc)` was the sibling pair §13a
          asks about and the first pass left as two adjacent bools —
          `LayerLeftValue` now, typed where the value is PRODUCED so a
          transposition has no brace to move with it. Proved by swapping the
          arguments and reading `error: cannot convert 'const bool' to
          'LayerLeftValue'`, then reverting
        * §9's SECOND GATE, which was missing: a converted mesh as the BASE
          under a field cutter, not only as the cutter (6.5 above)
        * `layer_scales_cleanly` refused only a soft PROFILE with a radius, so
          an EXTENDED composition — groove, shell, incise, pipe, the reliefs,
          which read `blend.k` as their own radius and ignore the profile —
          classified as a SIMILARITY with an absolute radius the scale does not
          reach, and clay.h promised otherwise. Fixed, documented at the setter
          and in placement.h, and the regression is at
          `clay_layer_placement_report` where §11 asked for it rather than one
          level below at the predicate: proved by reverting the extended term
          (six ops classify clean again, and the ABI reports SIMILARITY where
          GENERAL is required) and by reverting the composition term entirely
          (both ABI subcases fail, the control still passes). The ITEM-level
          twin of that hole is deliberately NOT touched and says so beside the
          code: it predates layer composition and reclassifies documents that
          carry no fold at all. THE OP LIST WAS ITSELF A HOLE — it left
          `Op::Paint`, which sits numerically below the extended range and
          spends `blend.k` as its colour falloff whatever the profile says —
          and is replaced in 7.10 by asking nothing about the op at all
        * document_fold_is_hard_union deleted, header corrected — it had no
          caller and `include/clay/scene/tape.h` said the three excluding entry
          points took it, where all three take `first_composed_fold_layer`
          (design.md §13f, item 1). Named unquoted here because the name is a
          deletion and not a claim, which is what check_task_symbols asks for
        * `fold_layers_below`'s `rev == now` comment stated the opposite of what
          the code does. Corrected at both ends (§13f, item 2)
        * `compile_layer_suffix`'s missing seam refusal: the ARGUMENT stated in
          the header and beside the code, and held by a new test — a seeded
          suffix across a composed seam is bit-identical to the whole document
          on three arms, and fails on all three when the seam is forced to a
          hard Add. THAT TEST COULD NOT REACH THE CASE THE ARGUMENT IS ABOUT:
          it passed no cull region, its cutter always had dabs, and its arms
          were unions and subtracts, each of which independently hides a seam
          the resume dropped. Closed in 7.10, so this entry records a promise
          that was half held rather than one that was
        * `first_composed_fold_layer`'s document-derived flag renamed off
          `have_acc` (7.1 above)
        * `Document.writable_at_minor` in pyclay, which had the composition
          setter/getter pair but not the query that decides whether the document
          they produce can be saved for an older build. pyclay is the surface
          every example and the gallery go through, and
          `examples/75_layer_booleans.py` builds exactly the document that
          cannot be written at 17. Returns `(ok, blocking_layer)` so the id
          survives the crossing; parity gate re-run with `--pyclay`
        * task 0.1's pricing benchmark, which design.md described and which did
          not exist (0.1 above)
      CONTRACT: 7.2 retitled from delegating its sweep to the review to
      recording that the sweep ran and what it found; 0.1, 6.5 and 7.1 extended
      with what this stage added; the scene-model delta gains the placement
      requirement and its scenario, which the code has had since the §11 fix and
      the spec did not say; design.md gains §13f and a correction beside the
      predicate it named

- [x] 7.9 THE GATE SWEEP, run whole rather than sampled: every gate CI runs
      against this change, re-run locally on the fourth review's tree, with the
      environmental failures separated from the real ones by reproducing them
      rather than by recognising them.
      GREEN, and each one was actually exercised:
        * `cmake --preset cpu-only -DCLAY_BUILD_TESTS=ON` + build, then
          `ctest --preset cpu-only`: 8 of 9, the ninth being pyclay_pytest,
          below
        * ASan + UBSan, which is the gate this change most deserved — it adds a
          C entry point with two capacity checks and a batch ceiling:
          `ctest --preset asan-ubsan` under
          `systemd-run --user --scope -p MemoryMax=32G`, 8 of 8, 1,984 s, no
          sanitizer diagnostic
        * ThreadSanitizer, which is not on the sweep's own list and was run
          because the same C entry point lands on the refill path the TSan job
          exists for: `setarch -R ctest --preset tsan`, 8 of 8, 728 s, and the
          string "ThreadSanitizer" appears nowhere in the log
        * check_layering, check_kernel_dialect, check_test_shards (2,454 cases
          partitioned across 4 shards, none duplicated, none unrun),
          check_task_symbols (340 claimed identifiers, all present, this entry's
          included),
          check_licenses, check_doc_latency, package_kernels --verify,
          check_swift_package, `openspec validate --all --strict` (38 passed)
        * the EXAMPLES job, which is not release_check's and is the only gate
          that runs `examples/75_layer_booleans.py`: the reference host session,
          then `CLAY_EXAMPLES_FAST=1 examples/run_all.py` (76/76 in 323 s), then
          check_gallery over what the run PRODUCED rather than over what is
          committed — OK, and the regenerated outputs discarded afterwards as
          the job does
        * the pyclay suite for real: 689 passed, 1 skipped, which is what
          exercises `Document.writable_at_minor` (7.8 above)
        * check_binding_parity in its STRONG reading — "imported <path>", not
          "parsed pyclay_module.cpp", which cannot fail (6.x above)
      ENVIRONMENTAL, reproduced rather than assumed, and none of them this
      change's:
        * pyclay_pytest under ctest, release_check's `bindings` and `abi` rows,
          and check_c_abi.py run bare: one cause, `GLIBCXX_3.4.31 not found`.
          ctest and release_check shell out to `sys.executable`, which is this
          box's anaconda, whose libstdc++ predates the one the module was built
          against. All four pass under `/usr/bin/python3`, or with
          `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6` where pytest is
          needed. Nothing in this change is in that path
        * release_check's `device` row: "engine changed since the gate ran",
          which is true and expected on a branch that is not a release
        * check_bench, which could not be run honestly at all: another project's
          gate had this box, and the load average went 1.7 -> 26 and 5.7 -> 31
          DURING two of the runs. Three full runs failed 1, 6 and 5 rows and the
          three sets barely intersect, which is what says interference rather
          than a regression — a real one fails the same row every time. The
          first run reported `BM_DeepDocRefillPlanned2000` at 6.3 ms against a
          4 ms ceiling while measuring the 10,000-item case at 3.4 ms, i.e. the
          same code costing twice as much on a fifth of the items. Every row any
          run failed was re-measured under a filter: 0.470 ms, 1.38 ms,
          0.148 ms, 6.76 ms, 0.80x and 0.12x, all inside their ceilings, slopes
          the right way up. Every row this change ADDED passed in all three
          loaded runs — BM_LayerFoldStack1000 at 0.65x, 0.72x and 1.06x its
          item-fold twin against a 1.5x ceiling, the layer-count slope at 1.00x,
          1.12x and 1.55x against 1.8x, `instrs` at 3,999 against 4,200,
          BM_BrickRefillLayersComposed resuming nothing and
          BM_BrickRefillLayersUnion at 0.00x it. This gate wants a quiet box and
          did not get one; nothing about it is evidence against the change
      CONTRACT: design.md carried two sections numbered §13e and two numbered
      §12c, each pair a parallel session colliding with a stage that had already
      taken the letter, and three references that no longer named one section.
      Three handovers recorded the collision and left it as another stage's
      text. Renumbered the later member of each pair (§13g, §12d), with a note
      under each saying what it landed as so an older handover's citation still
      resolves, and the three references pointed at the section they mean

- [x] 7.10 THE FIFTH REVIEW'S THREE MAJORS, each reproduced before it was
      touched and each closed with a test proved by reverting the fix.
      CODE:
        * THE FOURTH CULL-OBSERVABLE PREDICATE (design.md §13's general form),
          and it falsified a promise `include/clay/scene/tape.h` makes in
          writing. `Compiler::resume` returned early on `!chain_val &&
          cp.frames.empty()`, which skipped the seam's fold entirely — so
          `compile_layer_suffix` silently dropped a composed fold whenever the
          appended chain produced nothing IN THIS REGION. `chain_val` is
          cull-dependent and was being read as a document question. Reproduced
          on a clean build: a base sphere at the origin under a cutter whose one
          dab sits at x = 5, compiled for a region of ±1.5 — the checkpoint
          comes back `layer_have_acc=false doc_have_acc=true frames=0`, the
          suffix compiles to ZERO instructions, and over a 9³ lattice the
          seeded suffix matches the whole-document compile at 0 of 729 samples
          for Add, 0 of 729 for Subtract and **729 of 729 for Intersect**, worst
          3.4e37 (`CLAY_TAPE_FAR`), equal to the seed at all 729. Fixed by
          giving the seam ONE spelling: `emit_layer_fold` is now the whole rule
          — nothing beneath, an absent operand the operator ignores, an absent
          operand it reads — and `fold_layer` and `resume` both call it, so the
          early return has nothing left to decide and is gone. Scope, stated
          plainly: NOT reachable from the shipped C ABI, because every in-tree
          caller of `compile_layer_suffix` states `doc_have_acc = false` and
          `plan_resume`/`plan_frontier` refuse a composed seam through
          `layer_join_is_hard_union` first. It was a latent hole in a public C++
          contract, one caller change from being a wrong field per brick
        * THE FOLD'S ROUNDING SCALE had no coverage at all. Replacing
          `comp.rounding * layer_distance_scale(layer)` in `fold_layer` with
          `comp.rounding` and rebuilding left the whole suite green — 2,454
          cases, 16,464,984 assertions, 0 failed, the IDENTICAL assertion count,
          so nothing anywhere executed differently. No test in the suite
          compiled a document with a composed layer at a non-identity scale, and
          the only scaled-composition assertion queried `layer_blend_support`
          and never called `compile_document`. That term is what the SIMILARITY
          verdict rests on (§11, the spec delta, and the ABI subcase that
          asserts the CLASSIFICATION rather than the field)
        * A HARD-PROFILE PAINT CLASSIFIED AS A SIMILARITY, against the spec
          delta's "a radius is a radius whatever the blend profile says".
          `composition_radius_ignores_scale` read `profile != Hard ||
          op_is_extended(op)`, and `Op::Paint` is 3 where the extended range
          starts at 4, so it fell through both clauses — while
          `layer_blend_support` reported 0.3 and `document_cull_pad` 0.3 for the
          same layer, the engine padding for a radius the classifier said was
          not there. Swept every op the setter accepts at a hard profile and
          k = 0.3: Add, Subtract, Intersect and Paint all classified clean, all
          four reporting a support of 0.3. The predicate is now `c.blend.k >
          0.0f` and asks nothing about the op or the profile, which is the same
          field `chain_blend_support` reads and the end of a list to keep in
          step. Deliberately conservative for the three plain booleans, whose
          hard-profile `k` the kernel ignores: the cost is a recomputation on a
          scale gesture for a value that does nothing, against a picture that
          lags its own field. This hole is THIS change's; the item-level
          analogue stays deferred and says so beside the code
      TESTS, each proved by reverting its own fix:
        * `test_suffix_tape.cpp`, a NEW case — "a seeded suffix folds a composed
          seam whose layer this region drops" — asserting the fixture
          (`layer_have_acc` false, `doc_have_acc` true, no frames) rather than
          assuming it, then bit-identity against the culled whole-document
          compile on three ops, with teeth in both directions: the Intersect arm
          must MOVE every sample off the seed and the union and subtract arms
          must leave every one on it. Reverting the early return fails it with
          `-73 == 0` on the memcmp and `false == true` on the teeth, Intersect
          only. The existing composed-seam case gains two Intersect arms; those
          pass on both sides of the fix and are coverage, not evidence (§13d)
        * `test_layer_fold.cpp`, a NEW case — "a composed layer's rounding
          scales with the layer" — compiling `base_and_cutter` at scale 1 and 2
          against `ref_eval_document` over a lattice(24), on the four modes that
          READ rb (groove, tongue, relief, incise; `ctape_combine_dist` ignores
          the sixth argument for every other mode), with teeth that the scale
          moves the field and that the rounding is part of what moved.
          Reverting the multiplication passes at scale 1 and fails at scale 2 on
          all four arms — 420, 2412, 1000 and 1000 differing components
        * `test_layer_fold_sites.cpp`, the placement sweep widened from six
          extended ops to EVERY op the setter accepts, each checked both with a
          radius and without; plus two arms at `clay_layer_placement_report`,
          where §11 asks for the regression — a hard-profile Paint and a hard
          Add carrying a radius. Reverting the predicate fails four assertions
          in the sweep (Add, Subtract, Intersect, Paint) and both ABI arms
      CONTRACT: `tape.h`'s "the fold at the seam is EMITTED, not assumed"
      paragraph gains the half that was false — that it is emitted even where
      the cull leaves the layer with nothing in it — and says that both bullets
      are a C++ contract with no C-ABI caller today, which is what made the hole
      invisible. clay.h's "AN EXTENDED MODE COUNTS AS A RADIUS HERE" list, which
      enumerated nine ops and omitted `CLAY_OP_REPLACE`, is replaced by "ANY
      POSITIVE blend_k COUNTS AS A RADIUS HERE" plus the conservative note.
      design.md §13f and 7.8 above are corrected where they record as settled
      and held by a test something no test could see
