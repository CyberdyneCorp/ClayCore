# Proposal: Move Topological

## Why

`grab` weights its falloff by `|p - centre| / radius` — Euclidean distance
through SPACE. ZBrush's Move Topological weights it by distance along the
SURFACE, so pulling one finger does not drag the finger beside it.

Measured on two fingers 0.32 apart, joined only through a palm below them. A
world drag on the left finger at radius 0.30 leaves the right one exactly where
it was. At radius 0.50 — still less than the finger's own length — the right
finger's near edge is dragged from +0.158 to +0.074. Geodesically it is about
1.5 away: down one finger, across the palm and up the other. Euclidean distance
cannot tell those apart, and that is the entire brush.

## What it is

A **baked field operation**, the shape `relax` and `flatten` already have:
sample the source with the operation applied and hand back a `FieldVolume`.

The weight comes from a geodesic distance solved on a local grid:

1. A dense grid over a box around the anchor, sized to the drag's reach — the
   warp is the identity outside it, so nothing further needs solving.
2. Cells the source reports as material are the graph. Free space is not in it,
   which is what makes the distance topological: it cannot cross a gap.
3. Dijkstra from the anchor cell, 26-connected, edge cost the true step length,
   stopping past the radius.
4. The result is dilated a few cells outward, because the warp acts on space
   near the surface and a point just outside it still needs a weight.

Then the sample is `source(p - w(g(p)) * displacement)`, the same displacement
map `grab` uses, with `g` in place of `|p - centre|`.

## Why baked rather than a deformer

A deformer is a kernel function over a handful of floats. A geodesic field is a
solved grid; putting one in the tape would mean a deformer that reads the blob,
which no deformer does today and which every backend would have to grow. Baking
needs no kernel change and reuses the machinery relax and flatten established.

The cost is the one relax and flatten already pay and state: it BAKES, so the
result is a volume rather than a re-derivable edit, and chaining passes degrades
for the reason `add-flatten-modes` recorded. This is a single-gesture verb until
consolidation exists.

## What it is not

Not a change to `grab` or to `move_brush`. Euclidean Move stays the cheap,
exact-ish, chainable one; this is the expensive one you reach for when the form
has parts close in space and far along the surface.

Not mesh topology. There are no vertices here — "topological" means through the
material, which for a field is what connectivity is.
