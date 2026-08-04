# scene-model — bounds for lifted items

Delta for `add-profile-lifts`.

## ADDED Requirements

### Requirement: Influence bounds for lifted profiles
An item whose primitive is a lift SHALL compute its local bound from the profile: an extrusion bounds the profile's 2D extent across the extrusion depth, and a revolution sweeps the profile's radial extent into an annulus around the axis. Polygon profiles SHALL derive their extent from their vertices.

#### Scenario: Lifted item stays inside its bound
- **WHEN** the influence-bound property test runs on extruded and revolved items, including a concave polygon profile
- **THEN** band-clamped field values outside the bound are bit-identical with and without the item, and per-brick culled tapes stay band-clamp identical

#### Scenario: Revolved bound covers the full sweep
- **WHEN** a profile offset from the axis is revolved
- **THEN** the bound covers the whole circular sweep, not just the profile's own quadrant
