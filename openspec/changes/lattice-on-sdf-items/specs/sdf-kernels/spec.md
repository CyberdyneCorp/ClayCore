# sdf-kernels — a lattice cage on an SDF item

Delta for `lattice-on-sdf-items`.

## ADDED Requirements

### Requirement: A lattice deformer warps an SDF item through a cage
The kernel dialect SHALL provide a lattice (free-form deformation) deformer, so an item can be reshaped by dragging a few control points rather than by composing rotations and tapers.

Because a claycore deformer is an INVERSE point map and forward FFD has no closed-form inverse, the cage's control-point offsets SHALL BE the inverse warp: a point samples the undeformed field at `p + Bernstein(offsets, param(p))`. Newton-inverting per sample SHALL NOT be used, because iteration inside the evaluator breaks the single-source dialect the backends share; baking through a sampled volume SHALL NOT be used, because it ends the cage's editability.

The consequence SHALL be documented rather than implied, and SHALL NOT be described as `grab`'s character. The inverse cage is not the exact inverse of forward FFD: the two differ by a term proportional to how the basis VARIES along the displacement, so the error points the way the basis gradient does — over-travelling a drag toward rising weight and under-travelling one pointing away. `grab`'s weight always falls off along its drag, which is why that one always under-travels; a lattice does not inherit the sign. The magnitude SHALL be measured against the forward cage rather than asserted.

The cage SHALL store OFFSETS rather than positions, so an untouched cage is EXACTLY the identity with no special case, and material outside the box travels rigidly with the nearest part of the cage rather than being drawn onto it.

Evaluation SHALL be trivariate Bernstein, one formula for every cage size, with degree one less than the control-point count per axis — so two per axis is exactly trilinear, and the corner control points are interpolated.

Divisions SHALL be capped at four per axis. Unlike the mesh lattice, which evaluates once per vertex, this evaluates PER SAMPLE inside the raymarcher, at a cost of `nx * ny * nz` multiply-adds each time. The cap SHALL be stated where a caller meets it rather than discovered as a frame-rate cliff.

The cage SHALL live in the item's LOCAL space, as every other deformer does.

The declared Lipschitz factor SHALL be derived from the Bernstein DERIVATIVE bound — the control-point offset DIFFERENCES along each axis, scaled by the degree over the box's extent — and not from the offsets' magnitudes, which say how far the warp moves rather than how fast it varies.

The influence bound SHALL be the item's own bound grown by the largest control-point offset, since a Bernstein combination of the offsets cannot exceed the largest of them.

The item SHALL be marked inexact: a lattice is a bound field, not a distance.

It SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene whose cage is NOT uniform, so a backend that applied a translation but not the basis fails rather than passes.

#### Scenario: An untouched cage changes nothing
- **WHEN** a lattice whose offsets are all zero is compiled onto an item
- **THEN** the field is identical to the undeformed item's at every point

#### Scenario: A uniformly dragged cage translates the item
- **WHEN** every control point is offset by the same vector
- **THEN** the field equals the undeformed field translated by it, since the basis is a partition of unity

#### Scenario: The bound follows how fast the cage varies, not how far
- **WHEN** two cages have the same largest offset but different differences between neighbouring control points
- **THEN** the one whose neighbours differ more reports the tighter safe step scale

#### Scenario: A cage steep enough to fold degrades rather than lying
- **WHEN** neighbouring control points differ by more than the cage's own cell spacing
- **THEN** the compiled tape reports a step scale below one rather than claiming the field is a distance

#### Scenario: Material outside the box travels rigidly
- **WHEN** a point outside the cage's box is evaluated
- **THEN** it samples at the offset of the nearest point of the cage, and is not drawn onto the box

#### Scenario: Divisions are capped where a caller meets them
- **WHEN** a caller asks for more than four control points on an axis
- **THEN** the binding refuses, naming the per-sample cost as the reason
