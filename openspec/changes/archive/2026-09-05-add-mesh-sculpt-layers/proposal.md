# Proposal: mesh sculpt layers, and the detail that lives on them

## Why

A voxel layer can record a pass and dial it back. A mesh layer cannot, and an
SDF layer cannot either — `add-sculpt-layers` shipped the voxel half and left
its task 1.9 open, deliberately, because a diff of changed cells has no
counterpart in an edit list. The mesh side has no such excuse: a per-vertex
displacement is exactly the shape a layer wants, and once a subdivision
hierarchy stores detail relative to its parent, a layer is that detail split
into named, independently weighted contributions.

This is also the change that makes high-frequency detail a workflow rather than
a small brush. Wrinkles, scars, stitching, pores and grain are not "the same
brush at a smaller radius"; they are passes an artist adjusts as a pass — 80%
of the wrinkles, none of the damage, half the pores — which is a slider on a
recorded contribution and cannot be spelled with an undo stack.

Two words in the current vocabulary collide and the collision has to be settled
before either is exposed. `MeshBrush::Layer` is a brush ALGORITHM — deposit up
to a ceiling above the surface as it was at stroke start. A sculpt layer is a
persistent, addressable artist CHANNEL. They are unrelated, and shipping both
without distinguishing them in the API names is a support burden that never
ends.

## What changes

- **A sculpt layer stack on the multiresolution surface** — stable ids, names,
  strength, visibility, lock, reorder, remove, merge down, bake to base, and
  byte accounting, mirroring the voxel vocabulary where the mathematics allow.
- **Per-level layer detail**, because an anatomy pass belongs at a coarse level
  and a pore pass at a fine one, and forcing every layer to the finest level
  wastes the hierarchy the layers sit on.
- **Base deformation layers** at level zero, so a non-destructive proportion
  pass is possible and not only a non-destructive detail one.
- **A layer mask**, distinct from the temporary brush gate: the gate says where
  a brush WRITES, the mask says where a stored layer CONTRIBUTES.
- **A stroke transaction** writing into the active layer, with the same
  begin/stamp/commit/cancel shape the SDF sculpt transaction already uses.
- **Height and vector-displacement stamping** into detail, so an alpha shapes
  the surface rather than only scaling a weight.
- **An evaluated-detail cache** with block revisions, so a stamp on a deep
  stack does not sum every layer from zero.
- **Detail-aware smoothing and a detail eraser**, because a plain Laplacian
  smooth over pores removes the pores, which is rarely what was asked.

## Approach

Additive layer contributions, and say so rather than fake it. Voxel sculpt
layers replay cell writes, so their order matters and the spec pins which order
wins; additive displacement commutes, so ordering changes organisation and not
geometry. Reordering is still supported — artists expect it, blend modes may
arrive later, and a procedural layer may read the stack beneath it — but the
requirement states the current semantic fact instead of implying an order
dependence that does not exist.

A stroke on a layer shown at 50% records its FULL contribution; strength is
composition, not a scale on the pen. The alternative surprises the artist who
sculpts at low strength and later raises it to find their work underpowered.

Merge-down and bake-to-base are defined by visual parity — the evaluated
surface before equals the evaluated surface after — rather than by concatenating
coefficients, because the naive arithmetic divides by a lower layer's strength
and fails exactly when it is zero.

## Open questions

- **Whether colour gets a parallel stack.** Mesh paint and smear write vertex
  colours; a paint layer stack is the same idea and a separate change. Keeping
  geometry and colour layers separate in this one is the conservative call.
- **Procedural layers.** A global pore layer stored as parameters rather than a
  coefficient per vertex is a large memory win and a determinism obligation.
  The layer kind should be versioned from the start even if only sampled layers
  ship.
- **Whether layers can exist on a fixed-topology mesh with no hierarchy.**
  Mathematically yes — a sparse per-vertex offset needs no levels — and the
  question is whether that is a useful product or a second code path.
- **Prefix checkpoints.** A long stack is the same scaling problem a long SDF
  edit list had, and the answer there was a prefix cache. The cache keys should
  be designed so checkpoints are possible before they are needed.

## Impact

A new `mesh-sculpt-layers` capability. `scene-model` gains a history kind for
layer edits and for layer property changes — the property changes voxel sculpt
layers still do not record — plus memory rows. `file-io` gains persistence;
`c-abi` and `python-bindings` gain the surface. Additive: a surface with no
layers evaluates exactly as it did.
