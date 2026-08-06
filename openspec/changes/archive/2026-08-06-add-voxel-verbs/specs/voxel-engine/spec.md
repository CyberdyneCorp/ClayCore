# voxel-engine — the remaining sculpting verbs

Delta for `add-voxel-verbs`.

## ADDED Requirements

### Requirement: Fill cavities
The module SHALL provide a verb that fills pockets within a brush footprint: an empty cell with at least four of its six face neighbours occupied is inside a cavity rather than beside a surface, and a stated number of passes reaches that many cells deep. A hole passing all the way through the material, an open face, and a wide shallow dent SHALL be left alone — the verb fills pockets, and smoothing is the verb for surface irregularity.

#### Scenario: A pocket is filled
- **WHEN** fill-cavities runs over a slab with a one-cell pit two cells deep
- **THEN** the pit is filled and the rest of the slab is unchanged

#### Scenario: A through-hole is not filled
- **WHEN** fill-cavities runs over a slab pierced all the way through
- **THEN** the hole remains open

#### Scenario: A wide shallow dent is not a pocket
- **WHEN** fill-cavities runs over a dent two cells across and one deep
- **THEN** the dent is left alone, because it is surface irregularity rather than a cavity

### Requirement: Scrape
The module SHALL provide a verb that flattens the surface onto a plane and smooths it in one pass. Both decisions SHALL be taken from a single snapshot of the region, so that no cell's outcome depends on a neighbour the same call already changed.

#### Scenario: Scrape flattens and smooths together
- **WHEN** scrape runs over a bumpy slab
- **THEN** the surface is closer to the plane than before, and rougher features are reduced more than flattening alone reduces them

#### Scenario: Scrape is snapshot-consistent
- **WHEN** scrape runs twice over the same region from the same state
- **THEN** the results are identical

### Requirement: Smudge
The module SHALL provide a verb that drags surface material along a direction, leaving material below the surface where it was. This SHALL differ from grab, which translates every cell in its region: smudge smears a skin, grab moves a lump.

#### Scenario: Smudge moves the surface, not the body
- **WHEN** smudge runs across a thick block
- **THEN** the surface shifts along the direction and the block's interior is unchanged

#### Scenario: Smudge differs from grab
- **WHEN** smudge and grab run over the same block with the same displacement
- **THEN** the results differ

### Requirement: Carve with an alpha
The module SHALL provide a verb whose per-cell strength is modulated by a caller-supplied scalar grid, sampled by projecting each cell onto the plane perpendicular to a given direction. The engine SHALL NOT decode images: the caller supplies samples, a width and a height.

#### Scenario: The alpha shapes the carve
- **WHEN** an alpha grid that is opaque on one half and empty on the other is carved with
- **THEN** material is removed under the opaque half and left under the empty half

#### Scenario: A uniform alpha is the plain carve
- **WHEN** an alpha grid whose samples are all one is carved with
- **THEN** the result matches the same footprint carved without an alpha

#### Scenario: A malformed alpha is refused
- **WHEN** an alpha grid's dimensions do not match its sample count, or either is zero
- **THEN** the call is refused and the grid is unchanged

### Requirement: The new verbs honour mask and falloff
Every verb added here SHALL respect the brush's falloff, strength and optional mask exactly as the existing verbs do.

#### Scenario: A frozen region is spared by every new verb
- **WHEN** each new verb runs over a fully masked region
- **THEN** the grid is unchanged, while the same verb without the mask changes it
