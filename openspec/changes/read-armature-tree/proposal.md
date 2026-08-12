# Proposal: read a placed armature's tree back

## Why

A placed armature is write-only (#77). `clay_item_set_stroke_points` +
`clay_item_set_armature_parents` author a rig, and nothing reads either half
back: `clay_layer_stroke_points` — the readback #16 added — refuses the
primitive, and no call anywhere returns parents. A host that reloads a
document therefore holds the skinned surface and an unposable rig:
`clay_layer_armature_edit` takes node indices into a tree the host cannot see,
and branching topology is not recoverable from geometry — a shoulder and a
chest sphere equidistant from a neck parent either way, and the two produce
different subtrees under a move.

The c-abi spec already REQUIRED this: "Reading a placed armature's tree back
SHALL follow the size-query pattern the other variable-length readbacks use"
(add-armature). The requirement shipped without its implementation; this
change closes the spec-implementation gap and sharpens the requirement to name
the surface.

## What

Purely additive — the shape the armature requirement itself mandates ("no
existing signature changes"):

- `clay_layer_stroke_points` accepts `CLAY_PRIM_ARMATURE` and serves the
  geometry half: an armature's nodes are the same x, y, z, radius list the
  reader already returns for strokes and guides. The refusal narrows to prims
  that carry no point list.
- `clay_layer_armature_parents(doc, layer, node, out_parents, count)` serves
  the topology half by the same size-query pattern, counted in nodes so the
  two halves are parallel arrays. Mirrors the split on the setter side, and
  for the setter's reason: an armature IS a stroke plus a tree, and parents
  are the other half of a different primitive — not one nullable argument on
  the curve reader (the 0.26.0 grow-the-signature precedent covers optional
  attributes of the SAME answer, which parents are not).
- `clay_layer_node_prim(doc, layer, node, out_prim)` reports which primitive
  a placed node carries, refusing groups — the dual of `clay_layer_children`
  refusing items — so a reloading host finds its armature by asking rather
  than by probing readers until one stops refusing (#77's discovery half,
  the node-level shape of #69).

## What it does not touch

- **Serialization.** Parents already persist (format minor 7, file-io spec);
  the work is exposure, not plumbing. No format minor moves.
- **The scene model.** Everything read already exists on `scene::Node`.
- **The write side.** `clay_layer_set_stroke_points` still refuses a placed
  armature: replacing its points alone would desynchronise them from its
  parents, and `clay_layer_armature_edit` already owns that half.
- **Existing signatures.** Nothing changes shape; nothing renumbers.

## Impact

`c-abi` sharpens the armature requirement and gains the node-prim query.
`pyclay` needs nothing: the parity gate is C-reaches-what-pyclay-reaches, and
pyclay's builder-side readers were already exempt as builder state. Docs:
`docs/05-claycore-library.md` (the placed-node readback paragraph),
`docs/07-brushes-and-features.md` §6.
