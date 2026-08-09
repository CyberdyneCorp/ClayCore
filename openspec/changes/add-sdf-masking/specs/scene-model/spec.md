# scene-model — masks on the SDF side

Delta for `add-sdf-masking`.

## ADDED Requirements

### Requirement: An SDF layer can carry a mask
An SDF layer SHALL be able to carry a mask field, addressed in world units rather than in any layer's cell indices, so that one mask means the same thing on an SDF layer and a voxel layer and survives a resolution change.

#### Scenario: One mask, both representations
- **WHEN** a mask is painted against a voxel layer and then applied to an SDF layer covering the same world region
- **THEN** it selects the same world region in both

### Requirement: Mask-driven edits
The module SHALL provide at least two mask-driven edits: restricting a combine to the masked region, and extruding the masked region of the surface by a thickness, outward or inward.

Extrusion SHALL state how the wall at the mask boundary is formed, and SHALL declare a Lipschitz factor for it, because a mask-weighted offset makes the boundary's steepness a property of the MASK rather than of the geometry — a hard-edged mask would otherwise produce a wall the raymarcher cannot step through safely.

#### Scenario: An extruded mask raises only what it covers
- **WHEN** a masked region of a surface is extruded by a thickness
- **THEN** the surface outside the mask is unchanged, and the raised region stands proud by that thickness

#### Scenario: An empty mask changes nothing
- **WHEN** a mask-driven edit runs with a mask that selects nothing
- **THEN** the field is unchanged and the call succeeds
