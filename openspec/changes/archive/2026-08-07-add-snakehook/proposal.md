# Proposal: snakehook

## Why

The last of the core sculpting brushes with no equivalent here. Snakehook is how
a horn, a tendril, a finger or a spike gets pulled out of a form, and it is the
brush that makes a sphere into a creature.

## A correction to how this row was described

The roadmap called it "the largest remaining gap for a sculpting app… needs
geometry that GROWS along a drag rather than a stamp with fixed support, so
unlike the rows above it is not a rewrite of samples under a region."

The second half is right. The first half is wrong, and checking before writing
is what showed it: **a tapered stroke, smooth-unioned, already reads as a
snakehook tendril**, and the document's safe step scale stays at 1.0 — the field
is still exact, unlike a loft or a sweep. The stroke opcode sweeps a sphere
along a segment chain with a radius per point, which is a tendril if the radii
taper.

So this is not a new mechanism. It is a **resolver**: the same shape as
`cut_item`, where every ingredient existed and what did not exist was the step
that turns a gesture into an item. Leaving that step to each caller means each
one answers "where does it anchor", "how does the radius taper" and "how much
blend" differently, and a snakehook that detaches from the surface or beads
along its length is the result.

That makes the row much smaller than the roadmap predicted — and worth doing for
exactly that reason.

## What Changes

- **`brush::snakehook`**: a surface anchor, an inward normal and a drag path
  resolve into an ordinary stroke item, tapered along ARC LENGTH.
- **Anchoring**: the first point is placed inside the surface so the union is
  seamless. A tendril that starts on the surface leaves a visible neck; one
  that starts outside it detaches.
- **Arc-length taper**: a drag's samples are uneven, because a hand moves at an
  uneven speed. Tapering by sample index would make the radius depend on how
  fast the gesture was, which is the same defect the swept opcode had to avoid.
- **Python bindings** and an example.

## What this change does not do

- **It adds material rather than moving it.** ZBrush pulls existing surface, so
  the body dimples slightly where the tendril came from. This grows a tendril
  and leaves the body alone. The difference is visible only at the base, and
  conserving volume is a different brush, not a better snakehook.
- **A circular cross-section.** The stroke opcode sweeps a sphere. A tendril
  with a non-circular section is `Swept`, which already exists and takes
  profiles along a guide; a resolver for that is additive.
- **No automatic anchor.** The caller supplies the surface point and normal, as
  with the cut tool — it already has them from a pick. No camera and no picking
  enters this resolver.

## Capabilities

### Modified Capabilities

- `brush-engine`, `python-bindings`.

## Impact

- `include/clay/brush/stroke.h` and its source gain the resolver; the Python
  bindings, tests, docs, an example.
