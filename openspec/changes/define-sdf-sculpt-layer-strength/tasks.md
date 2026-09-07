## 1. The audit the milestone asks for

- [x] 1.1 `mesh::SculptLayerStack`: stores displacement coefficients, composes
      `E(n) = B(n) + Σ sᵢ · mᵢ(v) · Lᵢ(n, v)`. Additive, so it commutes and
      reordering is organisational
- [x] 1.2 `voxel::VoxelGrid`: stores a per-cell (before, after) difference.
      Partial strength is a reproducible dithered fraction of CELLS against the
      brush falloff's own hash. Replays writes, so it is order-DEPENDENT
- [x] 1.3 The two disagree about ordering and about what strength means, each
      correctly for what it stores. Neither generalises to the third
- [x] 1.4 Six of the roadmap's eight operations already exist on an SDF document
      layer; the missing two are merge-down and strength
- [x] 1.5 Since ABI 0.86.0 a layer folds with its own operator
      (`clay_document_set_layer_composition`), so "this pass carves what is under
      it" was already expressible per layer

## 2. The hard question

- [x] 2.1 REFUTED — the ray probe. Five ops, worst departure 0.000, and
      worthless: normal incidence with unit gradients on both fields makes the
      lerped crossing linear BY CONSTRUCTION. An instrument that cannot report
      non-linear reported linear five times
- [x] 2.2 Re-measured on occupied VOLUME, with a control built to be non-linear
      so the instrument is shown able to say no. Control read 0.187
- [x] 2.3 Union 0.329, subtract 0.136, smooth add 0.106. A union at slider 0.50
      delivers 0.17 of the pass
- [x] 2.4 Stable under refinement: 0.156 / 0.171 / 0.178 / 0.174 / 0.183 at
      n = 48 / 72 / 96 / 128 / 160, so it is not stair-stepping
- [x] 2.5 The two measurements together say WHAT is wrong: a lerp is a linear
      move on existing surface and a threshold on new surface, wearing one
      control

## 3. The decision

- [x] 3.1 Strength is defined on a quantity whose half is defined
- [x] 3.2 A deformation amplitude qualifies; a baked detail displacement
      qualifies and costs re-editability, which the artist chooses
- [x] 3.3 A pass of authored CSG nodes gets enable/disable, not a slider
- [x] 3.4 No ABI, no version bump — this change is the definition and its
      evidence

## 4. Not decided here

- [ ] 4.1 Merge/bake down for SDF layers. Both other stacks have it, it is not
      blocked on the strength question, and it should be its own change
