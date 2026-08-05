# sdf-kernels — region-weighted displacement

Delta for `add-region-deformers`.

## ADDED Requirements

### Requirement: Grab displaces a region of space with finite support
The tape SHALL carry a grab deformer that displaces the evaluation point by a vector weighted by distance from a centre, falling to zero at a radius. The weight SHALL follow an easing curve from the existing library, and the map SHALL be exactly the identity outside the radius so the deformer's influence stays local.

The item's local bound SHALL dilate by the displacement magnitude and no more. The tracked field SHALL downgrade to a bound with a Lipschitz factor of `1 + |d| · s / r`, where `s` is the easing curve's steepest measured slope — the same form `bend_linear` already uses.

#### Scenario: Only the region moves
- **WHEN** a grab is applied with centre c and radius r
- **THEN** the field at points beyond r from c is unchanged, and the surface within r has moved toward the displacement

#### Scenario: The falloff shapes the pull
- **WHEN** the same grab is applied with two different easing curves
- **THEN** the resulting surfaces differ within the radius and agree outside it

#### Scenario: Culling still holds
- **WHEN** the influence bound is computed for a grabbed item
- **THEN** every point whose field the grab changed lies inside it

#### Scenario: Front-facing only
- **WHEN** a grab is applied with the front-facing option against a stroke direction
- **THEN** surface facing away from that direction is left undisplaced, so the far side of a form does not move with the near side

### Requirement: Pose applies a transform across a region
The tape SHALL carry a pose deformer applying a rigid transform — rotation about a pivot, and translation — weighted by the same radial falloff, so a limb can be rotated about a joint with the influence tapering off along the form.

Pose SHALL report a Lipschitz factor accounting for the rotation's arc over the region, and SHALL offer the same front-facing option as grab.

#### Scenario: Rotation tapers across the region
- **WHEN** a pose rotation is applied about a pivot with radius r
- **THEN** geometry at the pivot is unmoved, geometry near the radius is fully transformed, and the transition follows the easing curve

#### Scenario: Sphere tracing stays safe
- **WHEN** a posed item is compiled
- **THEN** the tape reports a non-exact field and a safe step scale below 1
