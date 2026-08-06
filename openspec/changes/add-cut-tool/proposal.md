# Proposal: the cut tool

## Why

Drawing a shape over the model and cutting through it is, by the study's
account, the practitioners' "90% tool" — ZBrush's Trim Rect / Trim Circle /
Trim Lasso, 3DCoat's Cut Off. It is rated P0 there and described, for this
engine, as "pure EXTRUDE+op".

That description is right, and it is exactly the problem. Every ingredient
already exists: `Extrude`, 2D profiles for circle, box and arbitrary polygon,
`Subtract` and `Intersect`, and per-node rounding for bevelled walls. What is
missing is the step that turns *a shape the user drew* into that item — how
deep to sweep, which side survives, how the profile is oriented. An app can
write that today, and every app tool that cuts will write it slightly
differently. That is the same argument that justified the stroke engine.

## Two decisions worth stating

**The cut is a prism, not a frustum.** The obvious objection is that a shape
drawn on screen under a perspective camera sweeps a converging wedge, so a
"correct" cut should converge too. It should not. A converging cut has a cut
face that is not flat and a result that depends on where the camera was
standing — draw the same rectangle from two positions and get two different
solids. A trim is a straight cut. Both reference tools cut a prism, and this
one does too. It is the semantics, not an approximation we are settling for.

**No camera enters the engine.** The caller supplies a *cut frame* — an origin
and an orthonormal basis — which it already has, because it needed one to draw
the overlay. Shape coordinates are in world units on that plane, not pixels and
not normalized device coordinates: the engine has no viewport and should not
learn about one. What the engine owns is the part that is actually error-prone
and worth having exactly one answer to: sweeping far enough to cut all the way
through, orienting the profile, and what "keep the other side" means.

## What Changes

- **A cut frame and a shape** — rectangle, circle, or polygon — resolve to an
  ordinary edit item: an extruded profile, oriented along the frame, sized to
  clear the region being cut.
- **Resolution is pure.** Frame and shape in, item out, no document touched, so
  a UI can preview a cut before committing it and the result is testable
  without a scene.
- **The item is an ordinary item.** The caller places it through the existing
  path with whichever op it wants, so undo, stroke coalescing, `.clayspace`
  serialization, picking, masks and layer protection all apply unchanged.
- **Which side survives is the op, not a flag.** `Subtract` removes what is
  inside the swept shape; `Intersect` keeps only that. 3DCoat's "Shift =
  keep-outer" modifier is this choice and nothing more, so adding a separate
  parameter for it would be inventing a second way to say one thing.
- **The sweep extent is derived** from the region being cut, so a cut always
  goes all the way through. Supplying it by hand stays possible and is the
  only way to get a deliberate partial cut.
- **Bevelled cut walls** are the node's existing rounding.
- **Spline lassos reuse curve tessellation.** A closed curve drawn in the cut
  plane becomes a polygon profile through the machinery `add-curve-objects`
  just landed, rather than every app writing its own flattening.

## What this change does not do

- **No angled cut walls.** 3DCoat's depth falloff tapers the walls of the cut.
  That needs the cross-section to scale along the *sweep* axis; the engine's
  taper scales about Y while an extrusion runs along Z, so it would need a
  `taper_axis` deformer — the same relationship `elongate_axis` has to
  `elongate`, and about the same cost. Named here so the next person knows the
  price rather than discovering it.
- **No split.** Cutting a volume into two layers is two items with opposite
  ops on two layers: a recipe an app composes, not an engine feature.
- **No screen-space anything.** Pixels, viewports and projection matrices stay
  on the app's side of the boundary.

## Capabilities

### Added Capabilities

- `cut-tool`: the frame, the shapes, and resolution to an item.

### Modified Capabilities

- `python-bindings` and `c-abi`: the tool reaches both.

## Impact

- New `include/clay/cut/cut.h` + `src/cut/cut.cpp`, a `Quat::from_basis` in
  `math/transform.h`, both bindings, tests, docs, an example.
- ABI 0.16.0 — additive.
