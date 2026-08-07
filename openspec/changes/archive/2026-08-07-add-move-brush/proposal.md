# Proposal: the Move brush

## Why

ZBrush's Move drags the **surface**. On an SDF layer this engine cannot do that
today, and the reason is not that the deformation is missing — `grab` has been
there since `add-region-deformers` — but that nothing turns a world-space drag
into it correctly.

Three things stand between the two, and all three are the kind of geometric step
the cut tool and snakehook exist to keep out of every caller.

**A deformer is per ITEM, and its centre is in that item's LOCAL frame.** A
centre of `(0,0,0)` grabs a sphere sitting at world `x = 1.5`; a centre of
`(1.5,0,0)` does nothing to it. Measured, both ways.

**So grabbing one item of a blended form leaves the rest behind.** On two
smooth-unioned balls, adding a grab to the left one lifts its side by 0.070 and
the right by 0.000. A blocked-out sculpt is exactly this case, so the brush a
host writes naively is wrong in the normal case rather than an edge one.

**And a grab must be PREPENDED, not appended.** `Node::deformers` applies in
authoring order — the tape warps by `deformers[0]` first — so the first entry is
the OUTERMOST warp on the geometry and the last is closest to the primitive. A
grab appended after an existing deformer has its region weight evaluated at a
point that deformer already moved, so the drag lands somewhere the user did not
put it. This is the part not written down anywhere, and it is invisible until an
item has two deformers.

There is also nowhere to put the result. **The command vocabulary cannot change
a node's deformers at all** — `SetTransform`, `SetPrim`, `SetColor`,
`SetOpBlend` and `SetStrokePoints` exist, and there is no `SetDeformers`. A
deformer can only be set when the node is created, so a Move on an existing
sculpt is not expressible, let alone undoable.

## What Changes

- **`SetDeformersCmd`**: a whole-list replace, exactly as `SetStrokePointsCmd`
  is and for the same reason — a chain is a handful of records, so replacing it
  costs less than granular commands and its inverse is exact by construction.
  This is what makes a Move undoable and coalescable like any other edit.
- **`brush::move_brush`**: the resolver. A world centre, radius and
  displacement in; one `grab` per contributing item out, each already mapped
  into that item's own frame and marked to go at the front of its chain.
  Pure — nothing is read but transforms and bounds, so a host can preview a drag
  before committing it.
- **It maps, and it culls.** An item's world frame is `layer.xform * node.xform`
  — see the note below on why that is the whole story even for a nested node.
  Items whose influence bounds miss the grab sphere get no deformer at all: a
  warp with finite support, outside its own support, is a no-op that still costs
  a tape record on every evaluation.
- **The C ABI, Python bindings**, tests and an example.

## A group's transform does not reach its children

This was going to be the resolver's hardest part — accumulating the transform
chain from the layer down, so a node inside a transformed group mapped
correctly. It turns out there is no chain to accumulate: `compile_group` passes
the layer through unchanged and `emit_item` computes `layer.xform * item.xform`,
so **a group's own `xform` is ignored entirely**. Checked directly: a sphere
under a group translated to `x = 2` evaluates at the ORIGIN.

So the mapping is `layer.xform * node.xform` and nothing more, and a resolver
that helpfully accumulated the chain would disagree with the evaluator rather
than improve on it.

Worth saying plainly because it is a trap in its own right: a group carrying a
transform silently does nothing. That is not this change's to fix — it is either
a deliberate "a group is an op container, not a transform node" or a real gap,
and deciding which is a separate question — but a caller reading `Node::xform`
on a group would reasonably expect otherwise.

## Why this reconstructs a field-level grab exactly

Applying the same warp to every operand is not an approximation of warping their
combination — it *is* warping their combination. Combine ops are pointwise in
the deformed point, so `op(f(W(p)), g(W(p))) == (op(f,g))(W(p))` for any warp
`W`. And `Transform`'s scale is uniform by design, so a spherical falloff stays
spherical when mapped into an item's frame rather than becoming an ellipsoid.

Verified rather than argued: with the same drag mapped into both items of a
blended pair, the lift is symmetric (0.028 either side) and peaks at the world
centre (0.068), where grabbing a single item gave 0.070 and 0.000.

## What this change does not do

- **It does not make grab an exact translate.** `cgrab_point` samples where the
  material came from using the weight at the *sample* point, so a tip moves less
  than the displacement asked for — 0.31 for a drag of 0.5 over a radius of 0.8.
  That is `grab`'s documented, deliberate behaviour: the true preimage needs an
  iteration per sample and buys nothing a sculptor can feel. A Move brush
  inherits it, and says so rather than pretending otherwise.
- **No voxel change.** `VoxelGrid::sculpt_grab` is already a true region-level
  move; this closes the gap on the SDF side, which is where it was.
- **No stroke-driven Move.** This resolves ONE drag. Feeding it a stroke's
  stamps is the caller's, and is a row of its own if it earns one.
- **No new deformer opcode.** The point of this change is that none is needed.

## Capabilities

### Modified Capabilities

- `scene-model`, `brush-engine`, `c-abi`, `python-bindings`.

## Impact

- `include/clay/scene/commands.h`, `src/scene/commands.cpp` — the command, its
  inverse and its serialization tag.
- New `include/clay/brush/move.h`, `src/brush/move.cpp`.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`.
- New `tests/unit/test_move_brush.cpp`, `tests/unit/test_c_move_brush.cpp`;
  a new example and its `CAPABILITY_EXAMPLES` note; docs and roadmap.
