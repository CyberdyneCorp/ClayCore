## ADDED Requirements

### Requirement: A mesh cage is priced by the control points that were dragged
Evaluating a mesh lattice cage SHALL cost in proportion to the control points
that carry a non-zero offset, not to the number of control points the cage
holds. A control point at rest contributes exactly nothing to the offset field,
so the evaluation SHALL NOT visit it.

The per-axis basis SHALL cost O(n) in the divisions on that axis, so that a
cage of the largest size with one point dragged costs a small multiple of the
smallest cage with the same point dragged, rather than n^3 times as much.

The result SHALL agree with the full trivariate Bernstein sum over every
control point to float resolution, inside the box and past it where the
parameters clamp; Bernstein SHALL still interpolate the corner control points
exactly; and a cage whose every point has been returned to rest SHALL again be
exactly the identity.

#### Scenario: One corner of the largest cage
- **WHEN** one control point of a 32x32x32 cage is dragged and the cage is applied to a mesh
- **THEN** the evaluation sums one term per vertex, not 32,768
- **AND** the time per application is within a small multiple of the same drag on a 3x3x3 cage

#### Scenario: The sparse sum is the whole sum
- **WHEN** a cage of 2, 3, 8 or 32 divisions an axis has a few points dragged
- **THEN** the displacement at points inside, on and outside its box matches the full Bernstein sum over every control point to float resolution

#### Scenario: A point put back is forgotten
- **WHEN** a dragged control point is set back to a zero offset
- **THEN** it no longer counts as dragged, and a cage with none left is the identity
