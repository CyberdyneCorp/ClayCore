# voxel-engine — mask extrude

Delta for `add-mask-extrude`.

## ADDED Requirements

### Requirement: Mask extrude on a voxel grid
A voxel layer SHALL support the same extrude its SDF counterpart does, in cell space rather than by sampling a field: the masked cells of the source's surface, thickened by the requested amount, returned as a new grid carrying the source's colours.

It SHALL NOT go through a sampled field. A grid already knows which cells are on its surface, so resampling would cost a conversion and lose the palette for nothing.

The two representations SHALL agree: the same shape, mask and settings SHALL give extracts whose solid regions match to within a voxel, so that what a document means does not depend on which representation it is stored in.

The source grid and the mask SHALL both be left unmodified.

#### Scenario: A plate comes off a voxel ball
- **WHEN** a cap of a voxel ball is masked and extruded outward at a thickness
- **THEN** the new grid holds cells on that cap, roughly `thickness / voxel_size` deep, and none away from the mask

#### Scenario: Colour comes along
- **WHEN** a coloured region is extruded
- **THEN** the extract's cells carry the colours the source had under the mask

#### Scenario: The representations agree
- **WHEN** the same shape is extruded as an SDF field and as a voxel grid with the same mask and settings
- **THEN** the two solids occupy the same region to within a voxel

#### Scenario: The source survives
- **WHEN** an extrude is taken
- **THEN** the source grid and the mask are exactly as they were
