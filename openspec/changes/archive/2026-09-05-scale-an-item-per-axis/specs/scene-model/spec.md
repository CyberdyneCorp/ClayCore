# scene-model — scale an item per axis

Delta for `scale-an-item-per-axis`.

## ADDED Requirements

### Requirement: An item carries a per-axis scale
A placed item SHALL carry a per-axis scale in addition to the uniform one its transform already holds, so that a primitive whose extents differ per axis — a squashed capsule, a squashed cylinder, a box stretched on one axis — is expressible after placement and not only at creation.

The two scales SHALL MULTIPLY rather than replace one another: the transform's factor stays the uniform similarity factor and the per-axis scale modulates it, so setting either leaves the other where it was and the order they are set in does not matter.

The per-axis scale SHALL be applied INNERMOST — in the item's own local frame, inside its rotation and position — so that the composed map is `layer transform * item transform * per-axis scale`.

It SHALL NOT be a field of the transform type. That type is a SIMILARITY, and its algebra is closed because of it: the product of two is another and the inverse exists in closed form. A non-uniform scale does not commute with rotation, so widening the transform would make every composition in the engine a general matrix and take the exactness bookkeeping with it. Innermost and node-local, it composes as one matrix multiply at the places that build a matrix from an item.

Every component SHALL be greater than zero. A zero collapses the item onto a plane and has no inverse; a negative component mirrors it, which the layer mirror already expresses and which would flip the winding of a boolean without saying so.

#### Scenario: A primitive is stretched after it is placed
- **WHEN** a unit sphere is placed and given a per-axis scale of (2, 1, 1)
- **THEN** its surface crosses x at 2 and y at 1, and its geometry bound reports the same extents

#### Scenario: The two scales multiply
- **WHEN** an item is given a uniform scale of 2 and a per-axis scale of (1.5, 1, 1)
- **THEN** its effective scale is (3, 2, 2), whichever order the two were set in

#### Scenario: A degenerate scale is refused
- **WHEN** a per-axis scale with a zero or negative component is set
- **THEN** the edit is refused and the item is unchanged

### Requirement: A non-uniform scale costs exactness and not step size
Evaluating a per-axis scale SHALL divide the sample point by the three factors and multiply the resulting distance by the SMALLEST of them, which never overestimates the true distance.

The field SHALL therefore remain 1-Lipschitz, and the reported Lipschitz bound and safe step scale SHALL NOT move: a marcher takes the steps it always did and nothing gets slower. What the field SHALL lose is exactness — the value becomes a BOUND on the distance rather than the distance, short by at most the ratio of the largest factor to the smallest — and that loss SHALL be recorded in the compiled field's classification so a consumer that reads the value AS a distance can tell.

A per-axis scale whose components are all equal SHALL be treated as the similarity it is: the field stays exact, and it SHALL compile to tape identical to the uniform scale of the same factor. The default SHALL be `(1, 1, 1)`, so a document that never sets one SHALL compile to exactly the tape it compiled to before this existed.

#### Scenario: The reported value never overestimates
- **WHEN** the field of a squashed primitive is sampled from outside and a full step is taken along the inward ray by the reported value
- **THEN** the step never lands inside the surface

#### Scenario: Exactness goes and the step scale stays
- **WHEN** an item is given a non-uniform scale
- **THEN** the compiled field reports that it is no longer exact, while its Lipschitz bound and safe step scale are unchanged from the uniform case

#### Scenario: A uniform per-axis scale changes nothing
- **WHEN** an item is given a per-axis scale of (s, s, s)
- **THEN** the field stays exact and evaluates identically to the same item under a uniform scale of s

### Requirement: Every copy of an item is scaled with it
A layer's mirror and radial copies of an item, the sampled box of a feathered replace, and the gate that protects an item SHALL all compose the item's per-axis scale exactly as the item's own record does.

A copy that missed it would be a differently-shaped reflection of the same item; a gate that missed it would protect a region the surface no longer occupies. The bounds used for culling and for selection SHALL compose it for the same reason — a bound tight around the shape the item no longer is would let the cull drop something on screen.

#### Scenario: A mirrored squash is squashed on both sides
- **WHEN** a squashed item is placed in a mirrored layer
- **THEN** both copies have the same shape

### Requirement: Rounding follows the factor the distance follows
An item's rounding is authored in its own local units and converted to world units by the compiler. Under a per-axis scale that conversion factor SHALL be the uniform scale times the SMALLEST per-axis component — the same factor the compiled field multiplies the item's local distance by — and NOT the uniform scale alone.

The bound computed for culling and for selection SHALL use that same factor, so the dilation the bound applies and the dilation the field applies agree. They are separate code paths and agreeing is not automatic; an item whose bound dilated by more than its field does costs cull precision, and one that dilated by less drops geometry that is on screen.

#### Scenario: Rounding and its bound use one factor
- **WHEN** an item carrying a rounding is given a per-axis scale
- **THEN** the world-space rounding and the bound's dilation are both computed from the uniform scale times the smallest per-axis component
