## 0. Decide first

- [x] 0.1 SETTLED (design.md "Decision — task 0.1"): SPLIT AT THE LAST HARD
      BOUNDARY, which in this tree is the only boundary — the seam is always the
      last visible SDF layer, so the split stays available whenever THAT layer's
      composition is a hard Add, whatever the layers beneath it do, and
      `fold_layers_below` is left unchanged

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
- [x] 2.2 The first visible layer initialises and its op is NOT applied — the
      same `have_acc` rule items already follow, not a second one. It is the
      `if (have_acc)` guard on the combine and nothing else; the item rule's
      OTHER half (skip a carving item that opens a chain, seed a
      material-creating one) deliberately does not lift, because either would
      show nothing where the spec asks to show the layer itself
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

- [ ] 4.1 `compile_document_resumable`'s trailing union — PARTLY DONE in the
      fold stage, emission only: `Compiler::resume` now re-emits the ACTIVE
      LAYER's composition through the same shared emitter instead of a
      hard-coded `Op::Add`, derived from the `const Layer&` it already takes.
      That was not scope creep but the alternative to leaving `run()` and
      `resume()` emitting different fields for the same document. What is NOT
      done: the hand-built checkpoints in `bindings/c/clay_c.cpp`, the
      `test_tape_prefix_reuse.cpp:167` composed subcases, and the decision's
      header restatement
- [ ] 4.2 `compile_document_part` (`tape.h:372`)
- [ ] 4.3 `compile_document_except` (`tape.h:390`)
- [ ] 4.4 `tape_build.cpp:1281` — "a hard Add is exact and adds no extent"
- [ ] 4.5 `clay_c.cpp:1502` and `:1541` — the brick refill's multi-layer fold
- [ ] 4.6 Each DECIDED and tested, not discovered. A refill folding with the
      wrong operator returns a field that never existed and reports nothing

## 5. Invalidation

- [ ] 5.1 Composition joins the key of the tape, the cull index, the prefix cache
      (`layer_prefix_fingerprint`) and the brick seed store — PARTLY DONE in the
      model stage: it joins `digest::mix_layer_head`, so both
      `layer_prefix_fingerprint` and `layer_fingerprint` move, and it reaches
      the tape and the cull index for free through `apply_edit`'s revision
      bump. The brick seed store is deliberately NOT keyed on it (see the
      decision's ResumeKey note) and needs the split refusal instead, which is
      the fold stage's
- [ ] 5.2 Conservative first; narrow only with a measurement
- [ ] 5.3 Dirty influence for a moved, re-blended or hidden cutter

## 6. Gates

- [ ] 6.1 Hide/show a subtractive layer restores exact geometry
- [ ] 6.2 Order matters: A−B+C differs from A+C−B, stably across a reload
- [ ] 6.3 Old documents load unioning and render bit-identically
- [ ] 6.4 Undo/redo through the existing layer-property history
- [ ] 6.5 A converted mesh-to-SDF layer works as a cutter
- [ ] 6.6 Benchmarks at 10 / 100 / 1000 layers: a layer op costs about what the
      equivalent item combine costs, and there is no second evaluator
- [ ] 6.7 C ABI setter AND getter, pyclay, numbered example, version lines
