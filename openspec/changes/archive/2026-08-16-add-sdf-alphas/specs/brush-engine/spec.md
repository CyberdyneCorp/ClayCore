# brush-engine — alphas on SDF layers

Delta for `add-sdf-alphas`.

## ADDED Requirements

### Requirement: A stamp can be placed from a surface hit
The brush engine SHALL provide the placement a host needs to stamp an alpha where a user clicked: a centre, a direction and a tangent derived from a surface point and its normal, so a host does not recompute the frame and get a different answer from the one the engine would.

#### Scenario: A placement derived from a hit faces the surface
- **WHEN** a placement is built from a surface point and its normal
- **THEN** the stamp's direction is the normal and its tangent is perpendicular to it
