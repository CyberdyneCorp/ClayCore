# Proposal: document grab

## Why

ZBrush's Move drags the **surface**. claycore's `grab` deformer drags **one
item's own field, in that item's local space**. On a single-item form those
coincide, which is why the gap went unnoticed; on a form smooth-unioned from
several items — the normal case for a blocked-out sculpt — grabbing one item
pulls its share and leaves the rest behind. Measured on two blended balls,
grabbing the left lifts its side by 0.118 and the right by 0.022.

The workaround is to apply the same warp to every contributing item, mapping the
world drag into each item's frame. That is exactly the error-prone geometric
step `cut_item` and `snakehook` exist to keep out of every caller, and nothing
owns it here. Voxel grids already have a true region-level `sculpt_grab`, so
today the asymmetry is between the two representations rather than a limit of
the field.

## What it is

A **resolver**, the same shape as `cut_item` and `snakehook`: a world-space drag
in, a plan of ordinary edits out. No document is modified, so a host can preview
a Move before committing it.

It is exact rather than an approximation, for two reasons worth stating because
they are what make the row small:

- Combine ops are **pointwise in the deformed point**, so warping every operand
  identically is the same as warping their combination. Applying one world warp
  to every item IS a field-level grab, not an imitation of one.
- `math::Transform`'s scale is **uniform by design** ("non-uniform scale is an
  operator"), so a spherical falloff stays spherical under it and the mapping
  is always exactly expressible.

## The enabling gap

There is **no command that changes an existing item's deformer chain.** The
vocabulary has `SetTransform`, `SetPrim`, `SetColor`, `SetOpBlend` and the
stroke commands; deformers only ever cross it inside a whole `Node` in
`AddNodeCmd`. So a resolver returning per-item deformers would have no undoable,
serializable way to apply them, and a host would be reduced to removing and
re-adding each node on every frame of a drag.

`SetDeformersCmd` closes that. The scene-model spec already requires that "item
state carried by commands SHALL include any deformer chain, so deformed
documents round-trip" — this completes that intent for editing rather than only
for creation.

## Two decisions that fall out of the drag case

**Culling.** A grab breaks exactness and raises the Lipschitz bound, so putting
one on every item in a document would tank the safe step scale globally for a
local gesture. Only items whose influence bound actually intersects the grab
sphere get a deformer; the rest are provably untouched by it, since the warp is
the identity outside its radius.

**Coalescing, not appending.** During one Move drag the centre and radius are
fixed and only the displacement grows. Appending a deformer per frame would grow
the chain without bound and destroy both performance and exactness. The resolver
returns the **whole new chain** with a trailing grab of the same centre and
radius replaced rather than added — the same discipline `AppendStroke`/
`TrimStroke` coalescing already applies to strokes.

## What this is not

Not a new deformation: the kernel already has `cdeform_grab` and nothing about
it changes. Not a volume-conserving Move — the grab translates a region and does
not preserve volume, which is a different brush.

Symmetry is inherited rather than special-cased: a grab on an item carrying the
layer's mirror is mirrored with it, because the deformer is local and every
mirror copy evaluates the same local chain. That is the behaviour a symmetric
sculpt wants, and it costs nothing to get.
