## 0. Decide first

- [ ] 0.1 DECIDE what the resumable multi-layer split does when the fold is not a
      hard union — refuse, teach it the operator, or split at the last hard
      boundary. design.md §1 leans REFUSE for v1 because it is the only option
      that cannot be silently wrong. Settle it before any code

## 1. The model

- [ ] 1.1 `LayerComposition` on an SDF layer, using the EXISTING item enums
- [ ] 1.2 Accessors, with validation: enum range, finite floats, SDF layers only
- [ ] 1.3 A non-SDF layer REFUSES rather than storing dead state

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
