# brush-engine — one cage over a layer

Delta for `lattice-gizmo`.

## ADDED Requirements

### Requirement: A world-placed cage resolves into per-item lattices
The brush engine SHALL resolve one world-placed lattice cage into the per-item lattice deformers that reproduce it, so a gizmo cage acts on a layer's assembled form rather than on one item in its own frame.

This SHALL follow the resolver pattern the Move brush established: the layer is READ and never written, the warps are RETURNED rather than applied so a host can preview and so one command per node inside an undo group makes the gesture one undo step, and each warp SHALL belong at the FRONT of its node's chain, since the chain applies in authoring order and the first entry is the outermost warp.

Groups SHALL take no warp of their own; a group's transform does not reach its children in this scene model, so the children carry it.

An item's frame may be ROTATED, and a lattice box is axis-aligned by construction, so no per-item box reproduces a world-axis-aligned cage. The resolved deformer SHALL therefore carry a TRANSFORM and be exact, rather than resampling the cage onto a per-item grid, which would be an avoidable approximation.

#### Scenario: A cage over two items reaches both
- **WHEN** a cage is placed over a layer holding two items and one control point is dragged
- **THEN** both items receive a lattice warp expressed in their own frames

#### Scenario: A rotated item is warped exactly
- **WHEN** the same world cage is resolved onto an item rotated in its layer and onto an unrotated copy at the same world pose
- **THEN** both evaluate to the same world-space field

#### Scenario: An untouched cage resolves to nothing
- **WHEN** a cage whose control points have not been dragged is resolved
- **THEN** no warps are produced, because a chain of no-op deformers is worse than none

#### Scenario: Every item is reached, unlike a drag
- **WHEN** a cage is resolved over a layer holding an item far outside its box
- **THEN** that item still receives a warp, because a lattice's displacement outside its box is CLAMPED rather than zero and the material there travels rigidly

### Requirement: A lattice deformer may carry a transform
The kernel dialect SHALL provide a lattice deformer that maps the point into the cage's own space, warps it there, and maps it back — `p' = T⁻¹(T(p) + D(T(p)))` — so a cage placed anywhere in the world can be applied to an item in any frame.

It SHALL be a SEPARATE opcode from the axis-aligned lattice rather than a flag on it. The axis-aligned path SHALL pay nothing for the transformed one existing: adding per-sample work to a path that does not need it is the defect two prior changes had to undo.

The transform and its inverse SHALL both ride the blob, beside the offsets, rather than being derived per sample.

The declared Lipschitz factor SHALL be the one the untransformed cage reports, and the spec states why rather than leaving it to be rederived: the transform is rigid with uniform scale, so with `T = sR` the warp's Jacobian in the item's frame is `R⁻¹ J R`, similar to the cage-space Jacobian and therefore of the same norm.

The influence bound SHALL be grown by the largest control-point offset divided by the transform's scale, since a displacement bounded by that in cage space is bounded by that over the scale in the item's frame.

#### Scenario: An identity transform is the axis-aligned cage
- **WHEN** a transformed lattice whose transform is the identity is compared to the plain lattice with the same box and offsets
- **THEN** the two fields agree at every point

#### Scenario: The transform does not change the bound
- **WHEN** the same cage is applied with and without a rotation and uniform scale
- **THEN** the reported safe step scale is the same

#### Scenario: The axis-aligned path is untouched
- **WHEN** a document using only axis-aligned lattices is evaluated
- **THEN** it costs what it did before the transformed opcode existed
