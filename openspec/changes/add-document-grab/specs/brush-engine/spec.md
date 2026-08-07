# brush-engine — document grab

Delta for `add-document-grab`.

## ADDED Requirements

### Requirement: A world-space drag grabs the surface, not one item
The library SHALL resolve a world-space drag — a centre, a radius and a displacement — into a plan of per-item deformers that together drag the accumulated surface, rather than requiring the caller to place a deformer on one item.

The `grab` deformer acts on a single item's own field and takes its centre in that item's LOCAL frame. Those two facts make the obvious use wrong on any form built from more than one item: grabbing one item pulls its share and leaves the rest behind.

Resolving SHALL read the document but never modify it, so a caller can preview a Move before committing it.

#### Scenario: A multi-item form moves as one surface
- **WHEN** a world drag over a form smooth-unioned from several items is resolved and applied
- **THEN** every part of the surface within the drag's radius moves together, as it would if the form were a single item

#### Scenario: It agrees with a single-item grab
- **WHEN** the form IS a single item, and the same world drag is applied both through the resolver and as a hand-placed local grab
- **THEN** the two fields agree

#### Scenario: Resolving touches nothing
- **WHEN** a drag is resolved
- **THEN** the document is unchanged, and applying the returned plan is what changes it

### Requirement: The world-to-local mapping is exact
Mapping a world drag into an item's frame SHALL be exact rather than approximate. An item's world transform is its layer's transform composed with its own, and that transform's scale is uniform, so a spherical falloff stays spherical: the centre maps through the inverse transform, the displacement through the inverse rotation and scale, and the radius through the scale.

Applying one world warp to every operand is EQUAL to warping their combination, because combine ops are pointwise in the deformed point. The resolver therefore produces a true field-level grab, not an imitation of one.

#### Scenario: A transformed item is grabbed correctly
- **WHEN** a world drag is applied to a form whose items carry rotations, translations and uniform scales
- **THEN** the surface moves as the world drag describes, independently of how the items are individually placed

#### Scenario: The drag's centre is a world quantity
- **WHEN** the same world drag is resolved against two documents whose items are placed differently but whose surfaces coincide
- **THEN** the resulting surfaces coincide

### Requirement: A drag reaches only what it can touch
Only items whose influence bound intersects the drag's sphere SHALL receive a deformer. A grab breaks exactness and raises the Lipschitz bound, so placing one on every item would cost a whole document its safe step scale for a local gesture; and outside its radius the warp is the identity, so those items are provably unaffected.

#### Scenario: A distant item is left alone
- **WHEN** a drag is resolved against a document containing an item well outside its radius
- **THEN** that item receives no deformer, and the field around it is unchanged

#### Scenario: A local gesture stays cheap
- **WHEN** a drag reaching one item of many is applied
- **THEN** the document's safe step scale falls by no more than the same drag applied to that item alone

### Requirement: A drag coalesces rather than accumulating
During one drag the centre and radius are fixed and only the displacement grows. The resolver SHALL return the whole new deformer chain for each item, replacing a trailing grab that carries the same centre and radius rather than appending beside it.

Otherwise a drag would append one deformer per frame, growing the chain without bound and degrading both evaluation cost and the declared Lipschitz with every frame.

#### Scenario: A drag does not grow the chain
- **WHEN** a drag is resolved and applied repeatedly with a growing displacement
- **THEN** each item carries exactly one grab from that drag, and the surface follows the latest displacement

#### Scenario: A different drag is a different deformer
- **WHEN** a second drag with a different centre is resolved after the first is applied
- **THEN** the first drag's deformer is kept and the second is added beside it

### Requirement: A mirrored item is grabbed symmetrically
An item carrying its layer's mirror evaluates the same local deformer chain for every mirror copy, so a drag applied to it SHALL move the mirrored copies symmetrically. This is inherited from the local nature of the deformer rather than special-cased, and it is the behaviour symmetric sculpting wants.

#### Scenario: Symmetry follows the drag
- **WHEN** a drag is applied to a form whose items carry an active layer mirror
- **THEN** the mirrored side moves by the mirror image of the drag
