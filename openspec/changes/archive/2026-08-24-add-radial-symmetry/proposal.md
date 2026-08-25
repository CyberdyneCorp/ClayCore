# Proposal: radial symmetry as a layer mode, not an item modifier

## Why

The roadmap names this twice — in the gap table as "**Radial symmetry** | Three
mirror planes, no radial | Medium", and in the priority list as a P2 alongside
morph targets and instancing — and it has never been scoped. Issue #256 is the
filing.

Two things in the engine are radial-adjacent and neither is radial *symmetry*:

- **`Repeat::radial(count, offset)`** is a per-ITEM modifier. It arrays one item
  around Y with an O(2) evaluation — nearest sector plus angular neighbour — and
  that is a genuinely good property. It is the wrong shape for symmetry: it is
  set on an item, so it cannot make a *stroke* radial without the caller reaching
  into every node the stroke resolved into.
- **`Layer::mirror_axes` + `mirror_k`** is the real symmetry mechanism, and it is
  layer-level, evaluation-time, revocable, with a per-item opt-out and a blend
  seam.

So the two are asymmetric in design: **mirror is a layer mode, radial is an item
modifier**, and only the first gives symmetry's defining property — *the left arm
cannot drift from the right, because one node exists and evaluation reflects it*.
Anything radial today is authored as N hand-placed items that can drift.

## What changes

`Layer` gains `radial_count`, `radial_axis` and `radial_k`, and evaluation emits
the rotated copies the same way `tape_build.cpp` already emits mirrored ones.
Every surface `SetLayerMirrorCmd` carries comes with it: an undoable command,
serialization behind the scene minor, a C ABI entry point, pyclay parity, and a
`scene-model` requirement with scenarios.

## What this deliberately does not do

- **It does not replace `Repeat::radial`.** That stays, and stays the right tool
  for a 24-fold array: this mode multiplies emitted instructions per item by
  `count` where the modifier is O(2) forever. Sculpting radial counts are small.
  The design records the crossover rather than leaving a caller to find it.
- **No radial on voxel or mesh layers.** Neither has any symmetry today — the
  mesh sculptor has none at all, mirror included — and that is a separate gap
  which this change does not widen.
- **No gizmo.** The axis is layer-local, which is what already satisfies the
  roadmap's P0 criterion that a symmetry centre be explicit and persistent; a
  host that can move a layer can move the axis.
