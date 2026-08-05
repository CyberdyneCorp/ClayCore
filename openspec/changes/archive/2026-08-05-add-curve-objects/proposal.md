# Proposal: control-point curves

## Why

This is the roadmap's largest *structural* gap. A stroke today is a polyline:
every point is a hard corner, and the only way to get a smooth tube is to
supply enough points that the faceting stops showing. That is fine for a
finger drag, which is where strokes came from, and useless for authoring — you
cannot go back and adjust a shape made of two hundred baked points.

Curves are also an input, not only a shape. Tubes, arrays along a spline,
lofts and the whole parametric direction all need something to be *driven by*,
and the engine has nothing to hand them. `add-loft-opcode`, `add-swept-n` and
array-along-curve all wait on this row.

## The decision: a curve is not a new primitive

The obvious shape for this change is a `Curve` primitive with its own tape
opcode. That would be wrong, and expensively so: a new opcode means four
backend implementations, a parity corpus row, exactness and Lipschitz analysis,
and a spline evaluator running per sample point in the inner loop of every
raycast.

A curve is not a new thing to evaluate. It is a new way to *author the points
of something the tape already evaluates*. The stroke opcode already sweeps a
sphere of varying radius along a chain of segments, exactly and with finite
support. So:

- A stroke point gains a **type** — hard corner, spline, B-spline or Bezier —
  and, for Bezier, two handles.
- The compiler **tessellates** typed points into the same chain of segments the
  stroke opcode already reads.
- A point list where every point is a hard corner tessellates to itself, so it
  compiles to a **bit-identical tape**. That is the migration story and it is
  asserted, not assumed.

The consequence is that curves cost nothing at evaluation time and inherit
everything: four backends, per-brick culling, exactness tracking, picking,
undo, masks, the file format. A stroke is a curve whose points are all hard
corners — the same concept at two settings, like ghost and lock.

The tessellation tolerance is a **document property**, not a viewer setting.
Two builds must agree on what a document means, and a tolerance that varied per
host would make the field itself host-dependent.

## What this change does not do

- **No cross-section sweeps.** Swept-2, swept-N and loft need real profiles
  along a guide and stay their own rows. This change gives them the guide.
- **No curve tree.** 3DCoat's curves are workspace-global objects listed in
  their own panel. Here a curve is an edit item in a layer, like everything
  else, and a future generator references it by node id the way instancing
  references shared content. Adding a second kind of document citizen is a
  structural cost this change does not need to pay.
- **No radius profiles** (their Straight/Hemisphere/Cone/Muscle/Spike list).
  Those are a scalar function along the curve multiplying the interpolated
  radius; per-point radius already varies, so they extend this rather than
  needing it redesigned.

## What Changes

- **Point types**: hard corner, spline (Catmull-Rom), B-spline, Bezier with two
  local-space handles. 3DCoat's Bezier handles live in *screen* space and their
  own users call it a wart; local space is strictly better and costs nothing.
- **Closed curves**, for rings and lassos.
- **Adaptive tessellation** to a stated chord tolerance: subdivide while a
  span's midpoint deviates from its chord by more than the tolerance, to a
  bounded depth. Deterministic, and it honours the tolerance rather than
  approximating it with a fixed sample count.
- **A whole-list edit command**, so editing a curve is undoable and
  serializable like every other edit. A curve is tens of points, so replacing
  the list is cheaper than the bookkeeping six granular commands would need,
  and its inverse is exact by construction.
- **A version on the scene chunk**, threaded from the container's minor. Every
  additive change to a node has so far had to invent a packing trick to stay
  backward compatible; this is the general fix, and it makes the next one free.

## Capabilities

### Modified Capabilities

- `scene-model`: point types, closed curves, tessellation, the edit command.
- `file-io`: the versioned scene chunk and curve round trip.
- `python-bindings` and `c-abi`: curves reach both.

## Impact

- `include/clay/scene/types.h`, new `include/clay/scene/curve.h` +
  `src/scene/curve.cpp`, `src/scene/tape_build.cpp`, `src/scene/bounds.cpp`,
  `src/scene/commands.cpp`, `src/io/clayspace.cpp`, both bindings, tests, docs,
  an example.
- ABI 0.15.0 — additive. `.clayspace` minor 2; a minor 0 or 1 document loads
  with every point a hard corner, which is what it already meant.
