# sdf-kernels — pose with a line-gradient region

Delta for `add-pose-line-regions`.

## ADDED Requirements

### Requirement: Pose weighted along a line
The tape SHALL carry a pose deformer whose weight ramps along a segment: zero at the anchor, one at the end, taken from the point's projection onto the segment and shaped by an easing curve. The rotation SHALL be about the given axis through the anchor, so the anchor is a fixed point of the map.

Unlike grab and radial pose this deformer SHALL NOT be claimed to have finite support: the weight clamps, so material beyond the end anchor is fully rotated rather than untouched. Its bound SHALL therefore cover the swept arc — the hull of the item's bound and its fully-rotated image, dilated by the sagitta of the swept angle — rather than a dilation of the original.

The tracked field SHALL downgrade to a bound with a Lipschitz factor derived from the item's extent about the rotation axis against the length of the ramp.

#### Scenario: The anchor stays and the form bends
- **WHEN** a line pose is applied from an anchor to an end with a non-zero angle
- **THEN** the field at the anchor is unchanged and the form curves toward the direction of rotation, increasingly so with the angle. The weight is taken at the sample point rather than its preimage, so this is a bend rather than a rigid swing and the achieved rotation falls short of the nominal angle as it grows — the spec requires the bend, not the exact endpoint.

#### Scenario: The ramp follows the segment, not the distance
- **WHEN** two points lie equidistant from the anchor but at different projections along the segment
- **THEN** they receive different weights, which a radial region could not express

#### Scenario: The easing curve shapes the taper
- **WHEN** the same line pose is applied with two different easing curves
- **THEN** the fields differ between the anchor and the end

#### Scenario: The bound contains the swept geometry
- **WHEN** the deformed bound is computed for a line pose, including a large angle
- **THEN** every point of the rotated surface lies inside it

#### Scenario: Device agreement
- **WHEN** a line-posed item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance
