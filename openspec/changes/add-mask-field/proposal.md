# Proposal: a paintable mask field

## Why

Nothing in the engine can protect a region from an edit. Freeze is the tool
sculptors reach for constantly — hold this part still while I work next to it —
and it is also the substrate for masked shell, pose-by-selection, and region
operations generally. Pose already covers two of its three region sources; the
third is a mask reference, and it has been waiting on this.

3DCoat's version of this is their **worst-rated bug**: masks silently deactivate
after voxelization, to the point the community rule became "mask only in surface
mode". That failure is free for us to avoid, but only if survival across
resolution changes and representation bridges is a requirement from the start
rather than something bolted on. It is written into the spec below as an
invariant with its own regression test.

## The part that needs deciding: what "masked" means for an SDF

For voxels the answer is obvious — gate each cell edit by the mask.

For SDF it is not, and getting this wrong would be expensive. An SDF edit is a
declarative *item* in an ordered list, not a stamp applied once; it has no
per-point strength to attenuate. Masking an already-placed item at evaluation
time would mean a spatially-varying gate inside the tape — a transition-like op
per item — which destroys the rigidity and finite support that per-brick culling
depends on. That is a bad trade for a feature meant to be free.

So the mask gates edits **where they are authored, not where they are
evaluated**:

- **Voxel edits** consume it at apply time, per cell.
- **SDF edits** consume it when a stroke is turned into items — the stroke
  engine attenuates or skips stamps that fall in masked regions. That is what
  freeze means for a declarative representation: a frozen region receives no new
  edits. `add-brush-stroke-engine` is the change that lands it, and this one
  provides the query it needs.

The consequence, stated plainly so nobody expects otherwise: painting a mask
does **not** retroactively protect a region from items already in the list.
Freeze protects against what you do next.

## What Changes

- **`MaskField`**: a sparse scalar field in [0,1] on a chunked lattice, mirroring
  the voxel grid's storage so it inherits the same sparsity and serialization
  shape. Stored as `uint8` — 256 levels is more resolution than a falloff needs
  and keeps a masked layer cheap.
- **Per-layer, optional.** A layer with no mask behaves exactly as it does today
  and costs nothing.
- **Painted with the same brush vocabulary** as voxel edits: footprint, shape,
  falloff curve, strength. Masking should feel like sculpting because it is the
  same gesture.
- **Region ops**: invert, clear, expand, contract, smooth — the set Pose's stored
  selections need.
- **Sampling in world space**, so a consumer at any resolution can ask "how
  masked is this point" without knowing the lattice.
- **Survival as an invariant**: the mask is stored in world units, so changing a
  layer's voxel resolution or bridging between representations cannot silently
  drop it. Tested directly.

## Capabilities

### Modified Capabilities

- `voxel-engine`: mask storage, painting, region ops, and gating of voxel edits.
- `scene-model`: a layer may carry a mask.
- `python-bindings` and `c-abi`: the mask reaches both.

## Impact

- New `include/clay/voxel/mask.h` + `src/voxel/mask.cpp`, `include/clay/scene/document.h`, `src/io/clayspace.cpp` (a chunk for the mask), both bindings, tests, docs.
- ABI 0.12.0 — additive.
- Non-goals: evaluation-time gating of SDF items (see above), and mask-defined
  pose regions, which are a follow-up now that the query exists.
