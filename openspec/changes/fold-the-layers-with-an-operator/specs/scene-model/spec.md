## ADDED Requirements

### Requirement: Visible SDF layers fold under a per-layer operator

Each SDF layer SHALL describe how it combines with the accumulated field of the
visible SDF layers below it, using the SAME operators, blend profiles, blend
radii and rounding that an item uses. A layer boolean is the operation an item
boolean already is, so it SHALL NOT have its own vocabulary, its own evaluator or
its own kernel math.

**The first visible SDF layer SHALL initialise the accumulator and its own
operator SHALL NOT be applied.** Applying one against an empty field makes
`Subtract` and `Intersect` produce nothing at all, with no error, which is what
an artist who reorders their base layer to the top would otherwise see.

The fold SHALL remain symbolic. The field below a layer SHALL NOT be sampled into
a volume in order to combine with it: that would make an organisational act
destructive and would fix the document's resolution at whatever the fold chose.

A layer's own symmetry SHALL be resolved BEFORE it combines with what is beneath
it. Combining each mirrored or radial copy separately changes the result wherever
the blend is smooth, because a smooth combine does not associate.

Bounds SHALL follow the operator rather than defaulting to the union. A subtract
cannot create material outside its left operand and SHALL be bounded by it; an
intersect SHALL be bounded by the intersection; and every operator SHALL use the
bound its item-level equivalent already computes. A bound that is too small
loses ray hits and drops bricks from a plan, and both render as missing surface
rather than as an error.

Exactness and the Lipschitz bound SHALL fold exactly as the item-level combine
folds them, so that a document expressing a shape as two layers and a document
expressing it as one layer of two items agree in distance, colour, bounds and
safe step.

Layer order and visibility SHALL therefore be GEOMETRIC. Hiding a subtractive
layer SHALL restore the geometry it was cutting, and reordering layers SHALL be
capable of changing the shape.

A layer whose kind cannot enter the tape SHALL REFUSE a composition rather than
store one that does nothing, so that a control a host offers is a control that
acts.

A document saved before layer composition existed SHALL load with every layer
unioning, and SHALL render exactly as it did.

#### Scenario: A layer cuts the layers below it
- **WHEN** a visible SDF layer is set to subtract and sits above another
- **THEN** the document's field is the lower layers with that layer's shape removed, and hiding it restores the uncut geometry exactly

#### Scenario: Two layers and one layer agree
- **WHEN** a shape is expressed as layer A with layer B subtracting, and as one layer holding A then B subtracting, under equivalent transforms
- **THEN** the two documents agree in distance, colour, bounds and safe-step scale over many sampled points

#### Scenario: The stack's order is part of the shape
- **WHEN** the same three layers are ordered A−B+C and A+C−B
- **THEN** the two produce different geometry, and each is stable across saves and reloads

#### Scenario: The first visible layer is not applied against nothing
- **WHEN** the first visible SDF layer is set to subtract or intersect
- **THEN** it initialises the accumulator instead, and the document shows that layer rather than an empty field

#### Scenario: An old document is unchanged
- **WHEN** a document saved before this feature is loaded
- **THEN** every layer unions and the field is bit-identical to what that document produced before

#### Scenario: A non-SDF layer refuses a composition
- **WHEN** a composition is set on a mesh or voxel layer
- **THEN** it is refused, rather than stored and ignored
