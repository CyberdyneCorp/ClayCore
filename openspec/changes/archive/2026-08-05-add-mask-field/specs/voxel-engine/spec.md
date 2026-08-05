# voxel-engine — the mask field

Delta for `add-mask-field`.

## ADDED Requirements

### Requirement: A paintable mask field
The module SHALL provide a sparse scalar mask field in [0,1] on a chunked lattice, addressed in world units. It SHALL be paintable with the same brush vocabulary as voxel edits — footprint size, cube or sphere shape, falloff curve and strength — and SHALL support invert, clear, expand, contract and smooth over the painted region. Because the lattice is sparse and unbounded, invert is defined as flipping what has been painted rather than over an infinite complement.

The mask SHALL be sampleable at an arbitrary world position, so a consumer at any resolution can ask how masked a point is without knowing the lattice.

#### Scenario: Painting and reading back
- **WHEN** a mask is painted with a sphere brush and sampled at the brush centre and well outside it
- **THEN** the centre reads fully masked and the outside reads unmasked

#### Scenario: The falloff shapes the mask
- **WHEN** a mask is painted with a smooth falloff
- **THEN** values between the centre and the rim fall between fully masked and unmasked, rather than being binary

#### Scenario: Region operations
- **WHEN** a painted mask is inverted, and inverted again
- **THEN** it returns to its original values

#### Scenario: Empty masks are free
- **WHEN** a layer carries no mask
- **THEN** editing behaves exactly as it does without the feature and no storage is allocated

### Requirement: Masked voxel edits
Voxel edits SHALL accept an optional mask, and where one is given the effective edit strength at a cell SHALL be scaled by one minus the mask value there. A fully masked cell SHALL be left untouched by any edit.

#### Scenario: A frozen region survives an edit
- **WHEN** a region is fully masked and a brush is stamped across it
- **THEN** cells inside the masked region are unchanged and cells outside it are edited

#### Scenario: Partial masking attenuates
- **WHEN** a region is half masked and a brush is stamped across it
- **THEN** fewer cells change there than in the unmasked region, and more than in the fully masked one

### Requirement: Masks survive resolution and representation changes
The mask SHALL be addressed in world units rather than in a layer's cell indices, so that changing a layer's resolution, or moving content between the SDF and voxel representations, cannot silently discard or misalign it. This SHALL be verified by a regression test, not merely documented.

#### Scenario: A mask outlives a resolution change
- **WHEN** a mask is painted, the layer's voxel resolution is changed, and the mask is sampled at the same world positions
- **THEN** the values are unchanged

#### Scenario: A mask round-trips through the document format
- **WHEN** a document carrying a painted mask is saved and reloaded
- **THEN** sampling at the same world positions returns the same values
