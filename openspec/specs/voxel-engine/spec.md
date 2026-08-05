# voxel-engine Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Voxel storage
`clay::voxel` SHALL store colored voxel grids as palette-indexed dense chunks scaling to at least 256³ per layer, with palette + RLE compression for serialization. Voxel color SHALL be a palette index; palette edits SHALL recolor all referencing voxels without touching voxel data.

#### Scenario: 256³ grid within budget
- **WHEN** a 256³ layer with typical sparse occupancy is stored and serialized
- **THEN** in-memory chunks allocate only where voxels exist and the serialized form is palette+RLE compressed

### Requirement: Voxel editing operations
The module SHALL provide: set/erase/paint of single voxels and of brush footprints, box fills, line fills, per-axis mirror application of any edit, build-plane queries (which cell/face a ray hits at a given plane), and flood select (connected voxels by color/solidity).

Brush footprints SHALL support a shape: a solid cube, or the sphere of the same diameter. A brush of size N SHALL span exactly N cells per axis for every N ≥ 1, with the footprint running from `-((N−1)/2)` to `N/2` inclusive — symmetric for odd N, biased half a cell toward the positive axes for even N. The sphere SHALL admit cells whose centre lies within radius `N/2` of the footprint centre, so that it remains a subset of the cube and its occupancy ratio approaches π/6 as N grows. Cube SHALL remain the default.

#### Scenario: Mirrored edit
- **WHEN** a voxel is placed at (x,y,z) with X-mirror active
- **THEN** the mirrored voxel is also placed at (−x,y,z) in the same undoable command

#### Scenario: Flood select respects connectivity
- **WHEN** flood select starts on a voxel of color c
- **THEN** exactly the 6-connected voxels of color c reachable from the seed are selected

#### Scenario: Cube brush covers N cells per axis
- **WHEN** a cube brush of size N is stamped
- **THEN** it sets exactly N³ cells, for both odd and even N

#### Scenario: Every size is distinct
- **WHEN** brushes of size 3 and size 4 are stamped
- **THEN** size 4 covers strictly more cells than size 3

#### Scenario: Sphere brush
- **WHEN** a size-N sphere brush is stamped
- **THEN** only cells whose centre lies within radius `N/2` of the footprint centre are set, and the result is a subset of the cube brush of the same size

#### Scenario: Sphere is non-degenerate at small even sizes
- **WHEN** a sphere brush of size 2 is stamped
- **THEN** it sets a non-empty footprint

#### Scenario: Paint brush leaves empty cells alone
- **WHEN** a paint brush of either shape covers a region containing empty cells
- **THEN** occupied cells in the footprint are recoloured and no new cells are created

### Requirement: Greedy meshing
The module SHALL mesh voxel grids with greedy face merging, preserving per-face color (no color bleeding across merged faces), producing meshes suitable for both viewport preview and export.

#### Scenario: Greedy merge is lossless
- **WHEN** a voxel grid is greedy-meshed
- **THEN** the mesh's rasterized surface equals the per-voxel-face surface exactly (same faces covered, same colors), with strictly fewer or equal quads

### Requirement: Voxel–SDF bridges
The module SHALL expose a voxel grid as a step-function field usable in SDF compositing (with documented non-exact/bound classification), and SHALL rasterize any SDF layer into voxels at a caller-chosen resolution with nearest-color assignment.

#### Scenario: SDF to voxels round trip
- **WHEN** an SDF sphere is rasterized into voxels at resolution r
- **THEN** every voxel whose center is inside the sphere is set (and no voxel whose center is outside), with color sampled from the SDF color field

### Requirement: Brush falloff and strength
Brushes SHALL accept a falloff curve (constant, linear, smoothstep, gaussian) and a strength, producing a per-cell weight from the normalized distance to the footprint centre. Because voxel occupancy is binary, a weight between 0 and 1 SHALL be resolved by dithering against a deterministic hash of the cell coordinate and a caller-supplied seed, so that a weight of 1 always applies, a weight of 0 never applies, and intermediate weights give stable, reproducible fractional coverage.

#### Scenario: Constant falloff is the hard-edged brush
- **WHEN** a brush is applied with constant falloff and strength 1
- **THEN** every cell in the footprint is affected, matching the plain-size brush

#### Scenario: Falloff thins the rim
- **WHEN** a brush is applied with linear or smoothstep falloff
- **THEN** cells near the centre are affected and the proportion affected decreases toward the rim

#### Scenario: Dithering is deterministic
- **WHEN** the same falloff brush is applied twice to equivalent grids with the same seed
- **THEN** exactly the same cells are affected

#### Scenario: Strength scales coverage
- **WHEN** the same brush is applied at strength 1.0 and at strength 0.3
- **THEN** the lower strength affects strictly fewer cells

### Requirement: Sculpting verbs
The module SHALL provide sculpting operations that reshape existing material rather than stamping a footprint: smooth (majority filter over the 26-neighbourhood), inflate (dilate for positive amounts, erode for negative), flatten (pull the surface onto a plane), and pinch (draw surface cells toward the brush centre). Each SHALL respect the brush footprint, shape, falloff and strength, and SHALL be computed from a snapshot of the affected region so the result does not depend on the order cells are visited.

#### Scenario: Smooth removes an isolated spur
- **WHEN** smooth is applied over a single voxel spur protruding from a slab
- **THEN** the spur is removed and the slab is retained

#### Scenario: Inflate grows and erodes
- **WHEN** inflate is applied with a positive amount and then a negative amount of the same magnitude
- **THEN** the occupied count rises and then falls

#### Scenario: Flatten pulls the surface onto the plane
- **WHEN** flatten is applied to a bumpy surface with a plane normal
- **THEN** material above the plane within the footprint is removed and hollows below it that touch material are filled

#### Scenario: Pinch draws the surface inward
- **WHEN** pinch is applied to a slab
- **THEN** surface cells move toward the brush centre, and no cell outside the footprint is modified

#### Scenario: Order independence
- **WHEN** a sculpting verb is applied to a region
- **THEN** the result is computed from the pre-operation state, so no cell's outcome depends on a neighbour already modified by the same call

### Requirement: Voxel grab moves occupancy through the same map
The voxel engine SHALL provide a grab verb taking the same centre, radius, displacement and falloff as the SDF deformer, resampling occupancy and palette index from the inverse-displaced position so both representations mean the same thing.

Because occupancy is binary, resampling SHALL be nearest-cell, and the spec SHALL state plainly that a displacement larger than a cell aliases: material moves in whole cells, and a slow drag will step rather than flow. This is a property of the representation, not a defect to be hidden.

#### Scenario: Material moves with the pull
- **WHEN** a voxel grab displaces a region
- **THEN** cells in the direction of the displacement become occupied and cells behind it are vacated, with colour carried along

#### Scenario: Outside the radius nothing changes
- **WHEN** a voxel grab is applied
- **THEN** no cell beyond the radius from the centre changes occupancy or index

#### Scenario: Grab agrees with the SDF deformer in shape
- **WHEN** the same centre, radius, displacement and falloff are applied to a voxelized sphere and to the equivalent SDF sphere
- **THEN** the displaced surfaces agree to within the voxel size

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

