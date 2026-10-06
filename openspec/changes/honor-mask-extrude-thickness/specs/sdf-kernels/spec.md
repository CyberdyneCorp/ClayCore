## ADDED Requirements

### Requirement: Mask extrude thickness follows the surface
The library SHALL resolve a source field, a mask and a thickness into a new field holding only the masked patch of the source's surface, thickened. The mask SHALL select points on the source surface; its own depth away from that surface SHALL NOT cap the wall. The result SHALL be an ordinary sampled volume, so meshing, evaluation, picking, serialization and every backend apply to it unchanged.

#### Scenario: Thickness exceeds paint depth
- **WHEN** a thin mask on a sphere is extruded outward by 0.05, 0.1 or 0.6 world units
- **THEN** the wall height along each sampled surface normal is within 10% of the requested thickness
- **AND** the wall top remains even across the masked patch

#### Scenario: Anchoring the mask does not multiply source evaluations
- **WHEN** a dabbed mask on a 0.8 sphere is extruded outward by 0.05 at a 0.04 cell
- **THEN** the source is evaluated at most 1.5 times per lattice sample on average
- **AND** the result is bit-identical to projecting every sample onto the surface
- **AND** every sample two neighbouring bricks share holds the same value in both
