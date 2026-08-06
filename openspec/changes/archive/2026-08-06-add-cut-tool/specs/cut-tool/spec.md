# cut-tool

Delta for `add-cut-tool`.

## ADDED Requirements

### Requirement: A drawn shape resolves to an edit item
The module SHALL resolve a cut frame — an origin and an orthonormal basis — together with a 2D shape given in world units on that frame, into an ordinary edit item: the shape as a profile, extruded along the frame's sweep direction. Resolution SHALL be pure: it SHALL NOT read or modify a document.

The shapes SHALL include a rectangle, a circle and an arbitrary polygon. A frame whose basis is not orthonormal, or a shape with no area, SHALL be rejected rather than producing a degenerate item.

#### Scenario: A rectangle cuts a rectangular hole
- **WHEN** a rectangle is resolved against a frame and the item is subtracted from a solid
- **THEN** material inside the rectangle's sweep is gone and material outside it is untouched

#### Scenario: The cut is a prism, not a frustum
- **WHEN** the same shape is cut from two frames that differ only in how far the origin sits along the sweep direction
- **THEN** the resulting solids are the same, because the cut does not converge with distance

#### Scenario: A degenerate cut is refused
- **WHEN** a shape has zero extent, or a frame's basis is not orthonormal
- **THEN** resolution is refused rather than producing an item

### Requirement: The sweep covers the region being cut
The sweep extent SHALL be derived from the region the caller is cutting, padded so that a cut passes entirely through it rather than stopping inside. A caller SHALL be able to give the extent explicitly instead, which is how a deliberate partial cut is expressed.

#### Scenario: A cut goes all the way through
- **WHEN** a shape is cut from a solid deeper than the frame's own extent
- **THEN** the cut passes through both faces

#### Scenario: An explicit extent cuts only that far
- **WHEN** an extent shorter than the region is given
- **THEN** the cut stops there, leaving material beyond it

### Requirement: Which side survives is the op
Choosing between removing what the shape covers and keeping only what it covers SHALL be expressed by the op the caller places the item with — subtract for the first, intersect for the second — rather than by a parameter of the cut.

#### Scenario: Keep-outer and keep-inner are complementary
- **WHEN** the same cut item is subtracted from one copy of a solid and intersected with another
- **THEN** a point is inside exactly one of the two results

### Requirement: A cut is an ordinary edit
A resolved cut SHALL be an ordinary edit item, so that placing it is undoable, serializable, pickable and refused on a protected layer without the cut tool participating. Rounding on the item SHALL bevel the cut walls.

#### Scenario: A cut undoes like any other edit
- **WHEN** a cut is placed on a layer with undo enabled and then undone
- **THEN** the document is exactly what it was

#### Scenario: Rounding bevels the walls
- **WHEN** a cut is placed with a non-zero rounding
- **THEN** the cut's edges are rounded by that amount

### Requirement: A closed curve can be the cut shape
The module SHALL turn a closed control-point curve given in the cut plane into a polygon shape, using the same tessellation the engine uses for curves, so that a spline lasso does not require each caller to flatten its own curve.

#### Scenario: A spline lasso cuts a smooth outline
- **WHEN** a closed spline is converted to a cut shape and cut from a solid
- **THEN** the hole's outline follows the spline rather than its control polygon
