# Tasks: add-mesh-sculpt-layers

- [x] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): after
      `add-mesh-multires`, whose detail representation this extends. The two
      SHALL NOT be designed independently — a layer contribution and a base
      detail coefficient are the same quantity under two owners
      — `add-mesh-multires` verified landed in the tree; design.md D5 composes
      one `DetailField` per layer into the level's own rather than adding a
      second displacement representation

## 1. Decide first

- [x] 1.1 DECIDE the naming so the collision cannot ship: `MeshBrush::Layer` is
      a brush ALGORITHM (deposit to a ceiling above the stroke-start surface)
      and a sculpt layer is a persistent artist CHANNEL. Neither name changes
      meaning; the API SHALL make the difference unmissable
      — design.md D1: the channel is never spelled `Layer` unqualified
      (`SculptLayer*`, `clay_multires_sculpt_layer_*`,
      `surface.sculpt_layer(...)`), the brush enumerator is untouched, and
      `tools/check_c_abi.py` gains the rule so the discipline is gated
- [x] 1.2 DECIDE whether layers exist on a fixed-topology mesh with no
      hierarchy. A sparse per-vertex offset needs no levels; the question is
      whether that is a product or a second code path
      — design.md D2: yes as a product, no as a second path. A one-level
      `MultiresSurface` IS a fixed-topology mesh, and base deformation layers
      (2.6) are how it gets a stack; a bare mesh has no frame to measure an
      offset from
- [x] 1.3 DECIDE the layer-kind enumeration now, even if only sampled layers
      ship, so a procedural layer does not need a format break later
      — design.md D3: `SculptLayerKind : uint16_t`, `Sampled = 0` shipping,
      `Procedural = 1` reserved and REFUSED by the decoder, each layer payload
      length-prefixed so a later format can choose to skip deliberately
- [x] 1.4 DECIDE whether a colour layer stack is in scope. Recommendation: not
      here — mesh paint and smear write vertex colours and a paint stack is the
      same idea under different arithmetic
      — design.md D4: out of scope. Colour BLENDS and blending does not
      commute, so including it would make requirement 3.1 conditional on a
      layer's kind; and a level's colours are a rebuildable subdivided
      attribute cache, not per-level authoritative state

## 2. The stack

- [x] 2.1 `include/clay/mesh/sculpt_layer.h` — stable 64-bit ids, name,
      strength, visible, locked, kind, per-level detail, byte accounting
      — `SculptLayerId`, `SculptLayerKind`, `SparseWeightField`, `SculptLayer`,
      `SculptLayerStack`, `SculptLayerDelta` and `SculptLayerProperty`; per-layer
      `bytes()` and `coverage_vertices()`, stack `memory()`
- [x] 2.2 IDs are never vector indices. Reordering changes indices; an id is
      what a host, a serialized document and the C ABI hold
      — ids are minted from a serialized counter, `index_of` is a lookup, and
      `move_to`/`remove` never renumber anything a host holds
- [x] 2.3 Stack operations: add, remove, move, merge down, rename, set
      strength, set visible, set lock, set active
      — all nine on `SculptLayerStack`, each forwarded through `MultiresSurface`
      so the per-level sizes and the surface revisions stay in step and an undo
      record can be captured on the way
- [x] 2.4 A locked layer refuses a sculpt write; property changes may still be
      allowed and the rule is stated rather than discovered
      — `MultiresSculptor::stamp` refuses before the brush moves anything and
      `absorb_level_edit` puts the level's mesh back for a direct caller;
      `rename`, `set_locked` and `set_active` are still allowed and say so
- [x] 2.5 Evaluation: `base detail + Σ visible layer detail × strength × layer
      mask`, per level
      — `recompose_block` in `src/mesh/sculpt_layer_eval.cpp`; a layer at zero
      effective strength is SKIPPED rather than multiplied by zero, so an
      invisible stack composes to the base field bit for bit
- [x] 2.6 Base deformation layers at level zero, so a non-destructive
      proportion pass is possible and not only a non-destructive detail one
      — `State::BaseRestFrames` — the cage's rest frames, built lazily and only
      when a level-0 layer exists; `absorb_base_edit` subtracts the layer's
      contribution so sculpting the form under a proportion pass does not bake it
- [x] 2.7 Per-layer mask, sparse, DISTINCT from the temporary brush gate — the
      gate says where a brush writes, the mask says where a stored layer
      contributes
      — `mesh::SparseWeightField`, identity 1.0, same blocking and same block
      index as `DetailField`; the brush gate stays `field::MaskGate`
- [x] 2.8 Byte accounting per layer and per stack, and coverage per layer, so a
      strength change can dirty coverage rather than the model
      — `MultiresMemory::sculpt_layers` (authoritative) and `::composed`
      (rebuildable), plus `SculptLayer::coverage_vertices`

## 3. Semantics that must be written down, not discovered

- [x] 3.1 Additive layers COMMUTE. Reordering changes organisation and not
      geometry, and the requirement says so rather than implying an order
      dependence. This differs from voxel sculpt layers, whose replay of cell
      writes IS order-dependent and whose spec pins which order wins
      — composition visits a block's layers once and adds; `move_to` invalidates
      no block. `test_mesh_sculpt_layers` swaps two overlapping layers and
      compares the evaluated positions BIT for bit
- [x] 3.2 A stroke on a layer at strength 0.5 records its FULL contribution.
      Strength is composition, not a scale on the pen
      — `absorb_layered_detail` stores `ΔE = frame⁻¹(P_written) − E_before`;
      nothing in the change divides by a strength. Tested at 0.5 → 1.0
- [x] 3.3 Merge-down and bake-to-base are defined by VISUAL PARITY — evaluated
      surface before equals evaluated surface after — not by concatenating
      coefficients. The naive arithmetic divides by the lower layer's strength
      and fails exactly when it is zero
      — `SculptLayerStack::merge_down` sets the target to the identity it needs;
      `bake_sculpt_layer_to_base` is the same statement with the base as target.
      Parity tested at strengths 1.0, 0.37 and 0.0
- [x] 3.4 Removing a layer re-evaluates its coverage only; it does not replay
      strokes and does not touch other layers
      — `remove` notes the removed layer's coverage and nothing else; tested
- [ ] 3.5 Under symmetry, every mirrored write enters the SAME active layer and
      one undo step, with the coverage as the union
      — falls out of the transaction: the target layer is pinned at `begin`, so
      every mirrored stamp of one gesture enters that layer and one delta whose
      coverage is the union. Not yet asserted — the mirrored-stroke case belongs
      to `test_mesh_sculpt_layer_stroke.cpp` in the test stage

## 4. Writing into a layer

- [x] 4.1 `mesh::LayeredMultiresSculptor` with a stroke transaction —
      begin, stamp, commit, cancel — following the shape the SDF sculpt
      transaction already established
      — `mesh::LayeredMultiresSculptor` in `include/clay/mesh/layered_sculpt.h`
- [ ] 4.2 Cancel restores the layer exactly; commit produces ONE undo delta
      — `cancel` reverts the recorded `before` values rather than recomputing;
      `commit` hands over one `SculptLayerDelta` (or one `MultiresDelta` for a
      base-domain stroke). Exactness not yet asserted — test stage
- [x] 4.3 A hundred stamps over one vertex coalesce to one entry
      — `SculptLayerDelta::note_detail` keeps the FIRST `before` per (level,
      vertex) and `sync_after` rewrites the LAST `after`
- [x] 4.4 The active layer's blocks are writable; the evaluated lower stack is
      read-only and cached during the stroke
      — the composed field IS the cached lower stack, and
      `hold_sculpt_layer_composition` refuses a composition change while a
      stroke is open — which answers the design's open question
- [x] 4.5 Write domain: geometry at the active level, or detail relative to the
      subdivided parent, chosen explicitly by the caller. An automatic choice
      may be offered and SHALL NOT be the only one
      — `MultiresWriteDomain::{Automatic, Geometry, Detail}`, resolved once at
      `begin`; `Detail` with no active layer refuses rather than falling back
- [x] 4.6 An erase mode moves the active layer's detail toward zero and touches
      neither the base nor any other layer
      — `LayeredMultiresSculptor::erase` fades the ACTIVE layer toward zero and
      refuses when there is no target, so it can never reach the base
- [x] 4.7 Height stamps and tangent-space vector displacement, sampled through
      the SAME alpha sampler and orientation rules the existing mesh alpha
      uses, with image data borrowed and never copied into a preset
      — `include/clay/mesh/detail_stamp.h`; the placement is
      `kernel::calpha_frame` and the read is `kernel::calpha_sample`, the same
      two the scalar alpha uses. Images are planar and borrowed
- [x] 4.8 Vector displacement is interpreted in the tangent frame, never in
      world space — a world-space stamp is orientation-dependent and unusable
      over a curved surface
      — `DetailStampMode::Vector` returns three components in the vertex's
      transported frame; nothing is read or written in world space

## 5. Caching and scale

- [x] 5.1 Blocked detail storage, with the block size chosen by measurement
      — the stack shares `DetailField`'s blocking and its measured 1024-vertex
      default rather than choosing a second one — the shared block index is what
      makes 5.4 and 5.5 arithmetic
- [x] 5.2 An evaluated-detail block cache keyed on a stack revision; a rename
      SHALL NOT invalidate geometry
      — per-level dirty block sets on the stack; `rename` and `set_active` bump
      only `metadata_revision` and mark nothing. Tested
- [x] 5.3 Separate revisions for metadata, composition and content, so the
      three kinds of change invalidate what they actually affect
      — `metadata_revision`, `composition_revision`, `content_revision`, with
      `geometry_bumps` folded into the surface's own two so a pre-existing host
      keeps working
- [x] 5.4 THE GATE: a strength change on a layer touching a small fraction of a
      large surface costs its coverage, not the surface
      — tested: a layer inside one block of a five-block level recomposes exactly
      one block on a strength change
- [x] 5.5 THE GATE: a stamp on the top of a deep stack does not sum every layer
      beneath it over unrelated geometry. Prefix checkpoints if the measurement
      requires them; the cache keys SHALL be designed so they are possible
      — tested: sixteen layers over two disjoint blocks, a write into one visits
      the eight layers that reach it and none of the others
- [x] 5.6 Benchmarks over 1, 4, 16, 64 and 128 layers with local, overlapping
      and dense coverage
      — `BM_SculptLayerCompose*`, `BM_SculptLayerStrengthChange*` and
      `BM_SculptLayerStampOnStack*` over 1/4/16/64/128 with local, overlapping
      and dense coverage; the counters are the reading, not the clock
- [x] 5.7 Memory never silently stops recording. Report the budget and let a
      host merge, bake, delete or compact — a cap that silently stopped
      recording would leave the pass on the surface and un-dialable, which is a
      correctness bug wearing a memory limit's clothes
      — nothing is capped anywhere; `MultiresMemory` reports layer content apart
      from the composed cache and the comment says why a cap would be a
      correctness bug

## 6. Detail-aware verbs

- [x] 6.1 Smooth gains modes: geometry as today, detail-only, and
      preserve-detail. A plain Laplacian over pores removes the pores, which is
      rarely what was asked
      — `MultiresSmoothMode::{Geometry, DetailOnly, PreserveDetail}` on the
      layered sculptor; `PreserveDetail` smooths the form and folds the change
      into the level's own detail, leaving every layer's contribution intact
- [x] 6.2 A restore/morph mode — toward zero on the active layer, or toward the
      base — distinct from undo and documented as such
      — `erase` (the active channel toward zero) and `restore` (the level's own
      detail toward the pure subdivision); both are brushes and both are
      recorded as gestures that undo, which is what makes them not-undo

## 7. Undo, history, serialization

- [x] 7.1 `mesh::SculptLayerDelta` — layer id, level, changed entries or
      blocks, optional mask changes, with existence flags on each side
      — `mesh::SculptLayerDelta` — layer id, level, coefficient entries and mask
      entries, coalesced, with its own byte form
- [x] 7.2 Layer PROPERTY operations are undoable — rename, strength,
      visibility, reorder, lock, add, remove, merge, bake. Voxel sculpt-layer
      property changes are still outside the history; this is the change that
      does better rather than repeating it
      — `mesh::SculptLayerProperty`: scalar sides for rename/strength/visible/
      lock/active, and a whole-stack snapshot on each side for add, remove,
      move, merge and bake — plus the base detail and cage positions a bake
      wrote outside the stack
- [x] 7.3 `session::History` gains the kind and a resolver, through the same
      inversion the other kinds use
      — `Step::Kind::MultiresLayer` and `MultiresLayerProperty`, applied through
      `MultiresSurface::apply_sculpt_layer_{delta,property}` and the existing
      `set_multires_resolver`; no fifth resolver
- [ ] 7.4 Journal encode, decode, replay; older journals still replay; a
      malformed delta is refused
      — both journal kinds APPENDED so an older journal keeps its numbering;
      encode, decode and replay wired. The fuzzed-payload cases belong to
      `test_mesh_sculpt_layer_history.cpp` in the test stage
- [x] 7.5 Versioned serialization of the stack — id, name, kind, visible,
      locked, strength, per-level blocks, masks — inside the multires format
      rather than in the mesh stream
      — `kSurfaceVersion = 2`, the stack chunk inside the multires stream, and
      version 1 still accepted as a hierarchy with no layers
- [x] 7.6 A layer id survives a save, a load and a reorder
      — tested: save, load and a reorder before the save; ids and names survive

## 8. Bindings and gates

- [ ] 8.1 C ABI: layer ids as `uint64`, stack and property operations, layer
      info with `struct_size`, name retrieval into caller buffers rather than
      pointers into engine strings
- [ ] 8.2 C ABI: high-detail stamp descriptors, borrowed image data, changed
      block readback, revisions
- [ ] 8.3 pyclay, with a context manager for a stroke transaction — the voxel
      sculpt layer's `with grid.sculpt_layer(name):` is the precedent, and it
      is the only form that cannot leave a surface recording when a stroke loop
      raises
- [ ] 8.4 `tools/check_binding_parity.py` green
- [ ] 8.5 Swift smoke on macOS and in the simulator
- [ ] 8.6 THE MILESTONE, as a numbered example that renders and asserts: a
      wrinkle pass dialled 0 → 50% → 100% over a form that never changes, plus
      one layer removed with the others untouched
- [ ] 8.7 Version lines together; four presets green plus `release_check`;
      `tsan` under `setarch -R`; `check_layering.py` green
- [ ] 8.8 Docs: `docs/07-brushes-and-features.md` gains the stack and the
      distinction from `MeshBrush::Layer`; the README's sculpt-layer claim is
      widened from voxels to the representations that actually have them
