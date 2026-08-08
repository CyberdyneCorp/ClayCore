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

### Requirement: Repair reports before it repairs
The module SHALL provide a non-destructive query returning the number of enclosed empty regions, their total cell count, the largest one's cell count, and whether the grid is airtight. A caller SHALL be able to ask this without performing a repair.

#### Scenario: A hollow shell reports its void
- **WHEN** a hollow box's report is taken
- **THEN** it reports one enclosed void, of the size of the hollow, and not airtight

#### Scenario: A solid block is airtight
- **WHEN** a solid block's report is taken
- **THEN** it reports no enclosed voids and airtight

#### Scenario: A perforated shell is not enclosed
- **WHEN** a hollow box with a hole through its wall is reported
- **THEN** the interior is not counted as an enclosed void, because the outside reaches it

### Requirement: Close holes
The module SHALL provide a repair that seals perforations up to a stated radius by morphological closing over the whole grid. Material SHALL NOT be lost: a closing adds cells and removes none.

#### Scenario: A perforation is sealed
- **WHEN** close-holes runs with a radius wider than a pierced wall's hole
- **THEN** the hole is sealed and the interior becomes an enclosed void

#### Scenario: A large opening is left alone
- **WHEN** close-holes runs with a radius narrower than an opening
- **THEN** the opening remains

#### Scenario: Closing never removes material
- **WHEN** close-holes runs over any grid
- **THEN** every cell occupied before is still occupied

### Requirement: Fill voids
The module SHALL provide a repair that fills every empty cell not reachable from outside the grid's bounds. Reachability SHALL be over empty cells by face adjacency, seeded outside the occupied bounds, so that enclosure is decided rather than guessed at from local neighbourhoods.

#### Scenario: An enclosed void is filled
- **WHEN** fill-voids runs on a hollow box
- **THEN** the interior is solid and the report says airtight

#### Scenario: An open cavity is not filled
- **WHEN** fill-voids runs on a box with an open mouth
- **THEN** the cavity remains empty, because the outside reaches it

#### Scenario: Filling colours from the shell
- **WHEN** an enclosed void inside a single-coloured shell is filled
- **THEN** the filled cells carry that colour rather than an arbitrary palette entry

### Requirement: Repair honours the mask
Both repairs SHALL accept an optional mask and SHALL leave fully masked cells untouched, so that a frozen region is not repaired either.

#### Scenario: A frozen region is not repaired
- **WHEN** fill-voids runs with the void's cells fully masked
- **THEN** the void is left open

### Requirement: Voxels can magnify as well as pinch
Voxel layers have `sculpt_pinch`, which moves surface cells one step toward the brush centre. They SHALL also be able to move them one step away from it, so that the two representations agree on what the verb means — a document must not mean something different depending on which one it is stored in.

#### Scenario: Magnify moves surface cells outward
- **WHEN** a voxel shape is magnified about a point inside it
- **THEN** surface cells near that point move away from the centre

#### Scenario: It is the inverse of pinch
- **WHEN** a shape is pinched and then magnified with the same brush
- **THEN** the result is closer to the original than either operation alone

### Requirement: A mask can be inverted over a finite region
The mask SHALL support inverting over a caller-supplied region, and filling one with a constant. Over that region the result SHALL be `1 - value` for every cell in it, painted or not, and cells outside it SHALL be untouched. The boundary SHALL be the region the caller gave, never the storage's chunk grid.

Inverting a sparse unbounded lattice is not a representable operation, which is why `invert` is defined over what has been painted. That leaves the most common masking gesture — mask a region, invert, and edit everything else — unexpressible, because the complement a caller means is bounded by the model rather than by what has been touched.

`invert` SHALL keep its existing meaning: the two answer different questions, and a caller who worked around the unbounded one must not have it silently redefined.

#### Scenario: Everything else becomes masked
- **WHEN** a blob is painted, and the mask is inverted within a box containing it
- **THEN** the blob's cells read unmasked and the rest of the box reads fully masked

#### Scenario: The region's edge is the region's edge
- **WHEN** a mask is inverted within a box whose faces do not lie on the storage's chunk boundaries
- **THEN** the transition happens at the box's faces, and nothing outside the box changes

#### Scenario: Filling a region
- **WHEN** a region is filled with a value
- **THEN** every cell whose centre lies in it reads that value, and the field is empty again when the value is zero

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

