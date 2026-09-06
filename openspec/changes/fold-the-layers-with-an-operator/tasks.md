## 0. Decide first

- [x] 0.1 SETTLED (design.md "Decision — task 0.1"): SPLIT AT THE LAST HARD
      BOUNDARY, which in this tree is the only boundary — the seam is always the
      last visible SDF layer, so the split stays available whenever THAT layer's
      composition is a hard Add, whatever the layers beneath it do, and
      `fold_layers_below` is left unchanged

## 1. The model

- [ ] 1.1 `LayerComposition` on an SDF layer, using the EXISTING item enums
- [ ] 1.2 Accessors, with validation: enum range, finite floats, SDF layers only
- [ ] 1.3 A non-SDF layer REFUSES rather than storing dead state
- [ ] 1.4 Writing at a minor below 18 REFUSES a document carrying any non-default
      composition (design.md §7), and is allowed and byte-identical to what 17
      meant for a document where every layer unions. Not a silent degrade: a
      subtractive layer written as a union is a different model
- [ ] 1.5 A query a host can call BEFORE it saves: can this document be written
      at minor N without losing authored intent? Across the C ABI, with the
      refusal itself returning `CLAY_ERROR_UNSUPPORTED`

## 2. The fold

- [ ] 2.1 `run()` folds instead of unioning
- [ ] 2.2 The first visible layer initialises and its op is NOT applied — the
      same `have_acc` rule items already follow, not a second one
- [ ] 2.3 One low-level combine emitter shared with the item path; the kernel
      math stays single-source
- [ ] 2.4 A layer's symmetry resolves before it combines, once

## 3. Correctness

- [ ] 3.1 Bounds PER OPERATOR, from the item-level logic — subtract is bounded by
      its left operand, intersect by the intersection
- [ ] 3.2 Exactness and Lipschitz fold as the item combine folds them
- [ ] 3.3 THE PARITY FIXTURE: two layers vs one layer of two items, over many
      points, in distance, colour, bounds and safe step

## 4. The six sites that assume a hard union

- [ ] 4.1 `compile_document_resumable`'s trailing union
- [ ] 4.2 `compile_document_part` (`tape.h:372`)
- [ ] 4.3 `compile_document_except` (`tape.h:390`)
- [ ] 4.4 `tape_build.cpp:1281` — "a hard Add is exact and adds no extent"
- [ ] 4.5 `clay_c.cpp:1502` and `:1541` — the brick refill's multi-layer fold
- [ ] 4.6 Each DECIDED and tested, not discovered. A refill folding with the
      wrong operator returns a field that never existed and reports nothing

## 5. Invalidation

- [ ] 5.1 Composition joins the key of the tape, the cull index, the prefix cache
      (`layer_prefix_fingerprint`) and the brick seed store
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
