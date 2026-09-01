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

- [ ] 2.1 `include/clay/mesh/sculpt_layer.h` — stable 64-bit ids, name,
      strength, visible, locked, kind, per-level detail, byte accounting
- [ ] 2.2 IDs are never vector indices. Reordering changes indices; an id is
      what a host, a serialized document and the C ABI hold
- [ ] 2.3 Stack operations: add, remove, move, merge down, rename, set
      strength, set visible, set lock, set active
- [ ] 2.4 A locked layer refuses a sculpt write; property changes may still be
      allowed and the rule is stated rather than discovered
- [ ] 2.5 Evaluation: `base detail + Σ visible layer detail × strength × layer
      mask`, per level
- [ ] 2.6 Base deformation layers at level zero, so a non-destructive
      proportion pass is possible and not only a non-destructive detail one
- [ ] 2.7 Per-layer mask, sparse, DISTINCT from the temporary brush gate — the
      gate says where a brush writes, the mask says where a stored layer
      contributes
- [ ] 2.8 Byte accounting per layer and per stack, and coverage per layer, so a
      strength change can dirty coverage rather than the model

## 3. Semantics that must be written down, not discovered

- [ ] 3.1 Additive layers COMMUTE. Reordering changes organisation and not
      geometry, and the requirement says so rather than implying an order
      dependence. This differs from voxel sculpt layers, whose replay of cell
      writes IS order-dependent and whose spec pins which order wins
- [ ] 3.2 A stroke on a layer at strength 0.5 records its FULL contribution.
      Strength is composition, not a scale on the pen
- [ ] 3.3 Merge-down and bake-to-base are defined by VISUAL PARITY — evaluated
      surface before equals evaluated surface after — not by concatenating
      coefficients. The naive arithmetic divides by the lower layer's strength
      and fails exactly when it is zero
- [ ] 3.4 Removing a layer re-evaluates its coverage only; it does not replay
      strokes and does not touch other layers
- [ ] 3.5 Under symmetry, every mirrored write enters the SAME active layer and
      one undo step, with the coverage as the union

## 4. Writing into a layer

- [ ] 4.1 `mesh::LayeredMultiresSculptor` with a stroke transaction —
      begin, stamp, commit, cancel — following the shape the SDF sculpt
      transaction already established
- [ ] 4.2 Cancel restores the layer exactly; commit produces ONE undo delta
- [ ] 4.3 A hundred stamps over one vertex coalesce to one entry
- [ ] 4.4 The active layer's blocks are writable; the evaluated lower stack is
      read-only and cached during the stroke
- [ ] 4.5 Write domain: geometry at the active level, or detail relative to the
      subdivided parent, chosen explicitly by the caller. An automatic choice
      may be offered and SHALL NOT be the only one
- [ ] 4.6 An erase mode moves the active layer's detail toward zero and touches
      neither the base nor any other layer
- [ ] 4.7 Height stamps and tangent-space vector displacement, sampled through
      the SAME alpha sampler and orientation rules the existing mesh alpha
      uses, with image data borrowed and never copied into a preset
- [ ] 4.8 Vector displacement is interpreted in the tangent frame, never in
      world space — a world-space stamp is orientation-dependent and unusable
      over a curved surface

## 5. Caching and scale

- [ ] 5.1 Blocked detail storage, with the block size chosen by measurement
- [ ] 5.2 An evaluated-detail block cache keyed on a stack revision; a rename
      SHALL NOT invalidate geometry
- [ ] 5.3 Separate revisions for metadata, composition and content, so the
      three kinds of change invalidate what they actually affect
- [ ] 5.4 THE GATE: a strength change on a layer touching a small fraction of a
      large surface costs its coverage, not the surface
- [ ] 5.5 THE GATE: a stamp on the top of a deep stack does not sum every layer
      beneath it over unrelated geometry. Prefix checkpoints if the measurement
      requires them; the cache keys SHALL be designed so they are possible
- [ ] 5.6 Benchmarks over 1, 4, 16, 64 and 128 layers with local, overlapping
      and dense coverage
- [ ] 5.7 Memory never silently stops recording. Report the budget and let a
      host merge, bake, delete or compact — a cap that silently stopped
      recording would leave the pass on the surface and un-dialable, which is a
      correctness bug wearing a memory limit's clothes

## 6. Detail-aware verbs

- [ ] 6.1 Smooth gains modes: geometry as today, detail-only, and
      preserve-detail. A plain Laplacian over pores removes the pores, which is
      rarely what was asked
- [ ] 6.2 A restore/morph mode — toward zero on the active layer, or toward the
      base — distinct from undo and documented as such

## 7. Undo, history, serialization

- [ ] 7.1 `mesh::SculptLayerDelta` — layer id, level, changed entries or
      blocks, optional mask changes, with existence flags on each side
- [ ] 7.2 Layer PROPERTY operations are undoable — rename, strength,
      visibility, reorder, lock, add, remove, merge, bake. Voxel sculpt-layer
      property changes are still outside the history; this is the change that
      does better rather than repeating it
- [ ] 7.3 `session::History` gains the kind and a resolver, through the same
      inversion the other kinds use
- [ ] 7.4 Journal encode, decode, replay; older journals still replay; a
      malformed delta is refused
- [ ] 7.5 Versioned serialization of the stack — id, name, kind, visible,
      locked, strength, per-level blocks, masks — inside the multires format
      rather than in the mesh stream
- [ ] 7.6 A layer id survives a save, a load and a reorder

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
