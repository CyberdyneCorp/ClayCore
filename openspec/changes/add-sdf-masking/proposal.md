# Proposal: masks on the SDF side

## Why

Masking exists, and only for voxels. `MaskField` and `mask_extrude` are
`VoxelGrid` members; an SDF layer has no mask surface at all — not to apply
one, not to store one, not to drive an edit with one.

That asymmetry removes the workflow that hard-surface modelling is actually
built on. In every sculpting package the sequence is: mask a region, then
extrude it into a plate, or move it, or restrict the next operation to it.
Building `examples/35_hard_surface_helmet.py` there was no way to say "this
patch of the shell is a panel; raise it", so each plate had to be faked as a
separate `CutHollowSphere` on its own centre, trimmed inside its own layer.
That works and is a defensible technique — the example documents it — but it
is a workaround for a missing tool, and it scales badly: every panel is a new
layer and a hand-solved set of cutting boxes.

The voxel side already proves the shape of the answer. `mask_extrude` takes a
mask, a thickness and a side, and returns a grid. The gap is that the SDF side
has no equivalent and no mask to give it.

## Approach

Two pieces, and the first is useful alone.

**A mask an SDF layer can carry.** A scalar field over world space, reusing
`MaskField` so a mask painted for a voxel layer means the same thing on an SDF
one — masks are already addressed in world units rather than a layer's cells,
precisely so they outlive a resolution change.

**Operations that consume it.** At minimum: restrict an edit to the mask, and
extrude the masked region of the surface outward or inward by a thickness.
Extrusion is what turns a mask into a panel, so it is the one that closes the
hard-surface gap.

## The design question this hangs on

Extruding a masked region of a *field* is not the same problem as extruding a
masked region of a voxel grid, and pretending otherwise is how this goes wrong.

A voxel grid has cells, so "the masked surface, offset by t" is a set of cells
to fill. A distance field has no surface to move — only the field, whose zero
crossing is the surface. Offsetting the field by a mask-weighted amount raises
the masked region, but the boundary of the mask becomes a ramp whose steepness
is set by the mask's own gradient, not by the geometry. A hard-edged mask
therefore produces an infinitely steep wall the raymarcher cannot step through
safely, and a soft mask produces a bevel the artist did not ask for.

So the proposal must state how the plate's WALL is formed: whether the mask
carries an explicit border width, whether the wall is generated as a separate
skirt, and what Lipschitz factor the result declares. That is the design work,
and it is the reason this is a proposal rather than a task.

## Open questions

- Whether a layer carries one mask or several, and whether a mask is a document
  object with an id (which `.clayspace` would then persist beside the document,
  as voxel masks already are) or a per-edit argument.
- Whether restricting an ordinary combine to a mask is the same mechanism as
  extruding by one, or two separate features that happen to share a mask.
- How a mask is authored for an SDF layer at all. `apply_stroke` already
  resolves a stylus drag into stamps; painting a mask is the same gesture with
  a different target.

## Impact

`scene-model` gains the mask association and the mask-driven edits. `c-abi` and
`python-bindings` gain the entry points. `file-io` gains persistence if masks
become document objects. Additive throughout: no existing behaviour changes.
