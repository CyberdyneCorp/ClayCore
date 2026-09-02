# voxel-engine Specification

## Purpose
The representation for sculpting where topology must not constrain the artist:
sparse chunked cells, the sculpting verbs, the resolution level stack, painted
masks and surface groups, and the bridges in both directions between cells and a
field.

Its own capability rather than part of the scene model because what a voxel
layer guarantees is different in kind — an edit is destructive and local, there
is no edit list to replay, and undo is a recorded run of cell writes.

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
The module SHALL provide a verb that fills pockets within a brush footprint: an empty cell with at least four of its six face neighbours occupied is inside a cavity rather than beside a surface, and a stated number of passes reaches that many cells deep. An open face and a wide shallow dent SHALL be left alone — the verb fills pockets, and smoothing is the verb for surface irregularity.

A hole passing all the way through the material SHALL be left alone **where it is wider than one cell**. The rule counts face neighbours and nothing else, so it cannot distinguish a pinhole from a pocket: a one-cell perforation has its four lateral neighbours occupied and is filled, as close-holes fills a pierced wall. The exemption is a statement about width, not about topology.

A number of passes larger than the model's longest side SHALL be clamped to it. The working buffer is cubic in the pass count, so an unclamped value overflows the padded extent before the allocation is attempted, and past that side length every empty cell in the window has already been decided.

The verb SHALL decide from that local neighbourhood alone, which places it on freehand voxel work rather than on imported geometry: a dithered soft stamp, a narrow erase, magnify and grab all leave cells that satisfy the rule, while the void a boolean or a rasterized mesh leaves is sealed and wide, has no cell with four occupied face neighbours, and is fill-voids' work.

#### Scenario: A pocket is filled
- **WHEN** fill-cavities runs over a slab with a one-cell pit two cells deep
- **THEN** the pit is filled and the rest of the slab is unchanged

#### Scenario: A through-hole is not filled
- **WHEN** fill-cavities runs over a slab pierced all the way through by a hole more than one cell across
- **THEN** the hole remains open

#### Scenario: A one-cell perforation is filled
- **WHEN** fill-cavities runs over a slab pierced all the way through by a single-cell hole
- **THEN** the hole is filled, because a local neighbour count cannot tell a pinhole from a pocket

#### Scenario: An oversized pass count is bounded
- **WHEN** fill-cavities is asked for more passes than the model's longest side
- **THEN** it behaves as though asked for that many, rather than sizing a working buffer from the number given

#### Scenario: A wide shallow dent is not a pocket
- **WHEN** fill-cavities runs over a dent two cells across and one deep
- **THEN** the dent is left alone, because it is surface irregularity rather than a cavity

#### Scenario: A dithered soft stamp is the everyday input
- **WHEN** a soft stamp with a strength below 1 lays down material, so its dither leaves single-cell holes through that material, and fill-cavities runs over it
- **THEN** those holes are filled, and the enclosed-void count was zero throughout — they are open to the outside, so fill-voids would have reached none of them

#### Scenario: A sealed void is fill-voids' work
- **WHEN** fill-cavities runs over a hollow box whose interior the outside cannot reach
- **THEN** the interior is unchanged, because a local rule cannot see a wide sealed void, and fill-voids fills it instead

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

### Requirement: A deserialized grid's voxel size is a positive real
`VoxelGrid::deserialize` SHALL refuse a payload whose voxel size is zero, negative, infinite or not a number. Every world-to-cell conversion divides by that size and casts the result to a 32-bit integer, which is undefined for a non-finite quotient; a document loaded from disk is enough to reach it.

`MaskField::deserialize` SHALL apply the same rule to its cell size, infinity included.

#### Scenario: A grid declaring a zero voxel size
- **WHEN** a voxel payload carrying a voxel size of zero is deserialized
- **THEN** it is refused

#### Scenario: A grid declaring a non-finite voxel size
- **WHEN** a voxel payload carrying an infinite or not-a-number voxel size is deserialized
- **THEN** it is refused

#### Scenario: An ordinary grid round-trips
- **WHEN** a grid with a positive voxel size is serialized and read back
- **THEN** it round-trips losslessly, as before

### Requirement: Sparse operations cost the material, not its bounding box
Greedy meshing and the mask region operations SHALL cost time and memory in proportion to the cells that carry material, not to the bounding box enclosing them. A grid and a mask are sparse by construction and a bounding box is not: material far apart on two or more axes makes the box enormous while the material stays small.

Neither SHALL size a buffer from a difference of two lattice coordinates *taken across the whole grid*, which overflows a 32-bit integer for coordinates a deserialized grid or mask may legitimately carry, and would then ask for an allocation no allocator can satisfy — which ends the process rather than returning, since the library builds without exceptions. Slab grouping is what delivers this: a slice belongs to exactly one slab, so the window is sized from the chunks of that slab alone.

It does not cover the case where two chunks *of the same slab* sit at opposite ends of the lattice: the window is still their span, and a grid holding material at x = ±2^30 in one slab still asks for an allocation that ends the process. That is unchanged from before this requirement and is not addressed here; it is stated so the guarantee is not read as wider than it is.

Proportionality is not only about the window. Greedy meshing SHALL NOT pay a chunk lookup per cell probed: a chunk lookup is a hash and a map find, and the sweep visits every cell of the window once per face direction per slice, so a per-cell lookup makes a chunk holding one voxel cost the same as a full one. The mask build SHALL resolve the chunk once per chunk per slice and read that chunk's cells directly, and coordinates that fall in no chunk SHALL cost nothing rather than a lookup that returns empty. Only the neighbour probe on the far side of a face, on the slice at a chunk's own boundary, SHALL leave the chunk. Meshing SHALL NOT materialize a chunk for empty coordinates — a missing chunk is the normal representation of empty space, because a chunk that reaches zero occupancy is erased.

The cost SHALL therefore fall with occupancy: a grid of N chunks each holding a single voxel SHALL mesh in time proportional to N at a per-chunk cost far below that of a full chunk, and it SHALL stay linear in N rather than in the window the slabs span.

The output SHALL be unchanged. Merging still spans whatever the dense sweep merged, and a neighbourhood is still clamped at the padded bounding box rather than at any internal block edge. A change made for cost alone SHALL be byte-identical in the vertex and index buffers it produces, including their order, and SHALL be gated as such rather than by counts alone.

#### Scenario: Two voxels far apart mesh immediately
- **WHEN** a grid holding two voxels separated by thousands of cells on every axis is greedily meshed
- **THEN** it returns the twelve quads those two cubes expose, in time proportional to the two cubes

#### Scenario: Connected material merges as before
- **WHEN** a solid block is greedily meshed
- **THEN** each of its faces merges into the single quad it always did

#### Scenario: An almost-empty chunk costs less than a full one
- **WHEN** a grid holding one voxel in each of N distinct chunks is greedily meshed
- **THEN** the time is linear in N and the per-chunk cost is a small fraction of a chunk's worth of cells, rather than the same price a full chunk pays

#### Scenario: The mesh does not move
- **WHEN** the same grids are meshed by the previous implementation and by the faster one — a single cell, cells at negative coordinates, cells straddling a chunk seam on each axis, a rasterized blob, and every level of a multi-level grid
- **THEN** the vertex count, the index count and a hash of every attribute and index buffer are identical

#### Scenario: Meshing does not allocate empty chunks
- **WHEN** a sparse grid whose slab window spans mostly empty space is greedily meshed
- **THEN** its occupied-cell count and its serialized bytes are unchanged, and meshing it a second time yields the identical mesh — the sweep reads empty space, it does not create it

#### Scenario: Two painted blobs far apart expand immediately
- **WHEN** a mask carrying two small painted blobs separated by thousands of cells is expanded
- **THEN** both blobs grow, the space between them is untouched, and the call costs the painted cells

#### Scenario: A compact mask is not made slower
- **WHEN** a mask whose paint fits inside a single chunk is expanded
- **THEN** it costs no more than the region its paint occupies

### Requirement: Whether an edit changed anything is observable
A voxel grid SHALL expose a monotone count of the cell writes that actually changed a cell, since the grid was constructed. A changed cell SHALL mean a write that changed the stored palette index — empty to occupied, occupied to empty, or one index to another — so rewriting a cell with the index it already holds SHALL NOT be counted, and a palette recolour, which touches no voxel data, SHALL NOT move it.

The counter SHALL be maintained in the single mutation funnel every editing operation writes through, so that every brush, fill, sculpting verb, repair and rasterization is instrumented by construction rather than verb by verb.

The difference between two reads SHALL be zero exactly when the grid is byte-identical to what it was before the bracketed calls. That difference SHALL further be the exact number of changed cells for every verb that writes each cell at most once; for the two verbs that may write a cell twice within one call — pinch and magnify, which clear a cell and write its colour into a neighbour the same call may later visit — it SHALL be an upper bound, and the spec SHALL state so rather than claiming an exactness it does not have.

The count SHALL be monotone and never reset, so only differences are meaningful. It SHALL NOT be reachable through the occupied-cell count, which cannot answer the question: a verb that moves material without adding any leaves that count alone.

#### Scenario: A sub-cell grab is a valid edit that moves nothing
- **WHEN** a grab is applied with a displacement under half a cell on every axis
- **THEN** the call succeeds, the serialized grid is byte-identical, the occupied count is unchanged, and the change count is unmoved

#### Scenario: A sub-cell smudge is the same case
- **WHEN** a smudge is applied with a displacement under half a cell on every axis
- **THEN** the call succeeds, the serialized grid is byte-identical, and the change count is unmoved

#### Scenario: A supra-cell drag is distinguishable from a dead one
- **WHEN** the same grab or smudge is applied with a displacement over a cell
- **THEN** the serialized grid differs and the change count has moved

#### Scenario: The occupied count cannot tell the two apart
- **WHEN** a grab whose footprint is far wider than the material translates it rigidly by a cell, and the same grab is applied with a sub-cell displacement
- **THEN** the occupied count reads identically for both, while the change count separates them

#### Scenario: Rewriting a cell with what it already holds is not a change
- **WHEN** a cell is set to the index it already holds, or an already-empty cell is erased
- **THEN** the change count does not move

### Requirement: The grab dead zone is stated per axis
Because grab and smudge resample nearest-cell, the rounding SHALL be documented as PER AXIS: a displacement whose length exceeds half a cell can still round to zero on every axis and move nothing. Half a cell on the largest component SHALL be documented as a necessary and not a sufficient condition, because the falloff shrinks the pull away from the brush centre and the front-only gate, which is half weight on the plane through the centre, doubles the dead zone there.

#### Scenario: A drag shorter than a cell on every axis is dead
- **WHEN** a grab is applied with 0.4 of a cell on each of the three axes, a pull 0.69 of a cell long
- **THEN** every cell resolves to itself and nothing moves

#### Scenario: front_only widens the dead zone
- **WHEN** a displacement that moves material with the gate off is applied with the front-only gate on
- **THEN** it may move nothing, because the gate halves the pull at the brush centre

### Requirement: A grid reports which chunks changed
A voxel grid SHALL track, per resolution level, the set of chunks whose meshed surface a mutation could have changed, and SHALL expose that set through a DRAIN — a call that returns the set and empties it in one step.

The set SHALL be fed from the single cell-write choke point every public verb funnels through, so that a verb added later reports without being told to. A write that does not change the cell SHALL dirty nothing, because nothing about the surface moved; this is the same distinction the change counter already draws, and it is what keeps the set proportional to the edit rather than to the brush footprint.

A write on a cell lying on a chunk FACE SHALL also dirty the chunk across that face when that chunk exists, because the exposure test reads the neighbour cell and the neighbour's quads therefore depend on this write. A neighbour chunk that does not exist SHALL not be dirtied: an absent chunk holds no material and emits no quads, so the set stays proportional to the material. Missing this rule is a hole in the surface that appears only at chunk boundaries.

A chunk that reaches zero occupancy is ERASED from the grid. It SHALL still be reported dirty, or the quads it contributed to a host's buffer are never removed.

Propagation across resolution levels writes cells at other levels, and those writes SHALL dirty those levels' sets by the same rule, so that a level's set describes that level whether or not it was the active one when the edit landed.

The drain SHALL be idempotent in the sense that matters: draining twice with no write in between yields the set and then nothing. A write that lands after a drain SHALL appear in the next drain, never in the one already returned. A set that is never drained SHALL cost the host nothing beyond memory — a whole-grid mesh SHALL ignore it entirely and SHALL NOT clear it, so "mesh everything once, then track" and "track from the start" reach the same state.

The order of the drained keys SHALL be deterministic for a given set, so that two runs of the same edit sequence mesh in the same order.

#### Scenario: Every mutation verb reports what it touched
- **WHEN** each public mutation — set, erase, paint, the brush and fill forms, the sculpting verbs, the repair passes, mirror, stroke application and rasterization — is applied to a grid and the dirty set is drained
- **THEN** the drained keys include every chunk holding a cell the call changed

#### Scenario: A face write dirties the neighbour across the seam
- **WHEN** a cell on a chunk's boundary face is written, and material exists in the chunk across that face
- **THEN** both chunks are drained as dirty, because the neighbour's exposed faces changed

#### Scenario: A chunk emptied to nothing is still reported
- **WHEN** the last occupied cell of a chunk is erased, so the chunk is dropped from the grid
- **THEN** that chunk's key is drained as dirty, so a host can remove the quads it used to contribute

#### Scenario: An edit that changes nothing dirties nothing
- **WHEN** a cell is written with the value it already holds, or a verb runs with no effect
- **THEN** the drained set is empty

#### Scenario: A drain empties the set
- **WHEN** the dirty set is drained twice with no write in between
- **THEN** the first drain reports the chunks and the second reports none

#### Scenario: A write after a drain lands in the next one
- **WHEN** a cell is written, the set is drained, and another cell in a different chunk is written
- **THEN** the second drain reports the second chunk and not the first

#### Scenario: A whole-grid mesh leaves the set alone
- **WHEN** a grid with a non-empty dirty set is meshed whole
- **THEN** the mesh is exactly what it would be with an empty set, and the set is still there to drain

### Requirement: A grid meshes a named set of chunks
A voxel grid SHALL mesh a caller-named list of chunk keys, producing the quads those chunks' cells expose and nothing else, and SHALL report per key what that key contributed as a contiguous vertex range and a contiguous index range.

The exposure test SHALL read the neighbour cell wherever it lives, including in a chunk the caller did not name and in a chunk that does not exist. A cell whose neighbour across a face is occupied has no face there whether or not the neighbour was requested, so the SURFACE a regional mesh describes SHALL be exactly the surface the whole-grid mesh describes over the same chunks: the same exposed faces, the same covered area, the same colours.

Greedy quads are axis-aligned and exact, and a voxel face belongs to exactly one cell, which belongs to exactly one chunk. So clamping the merge to a chunk boundary SHALL produce MORE, SMALLER quads over the identical surface and SHALL NEVER produce a crack. A regional voxel mesh therefore SHALL NOT attribute straddling geometry to a requested key, which is what a marching-cubes brick mesh must do (#66) and what makes that one's ranges share vertices; here the ranges SHALL PARTITION the mesh with no vertex shared between two keys.

A named key that holds no chunk SHALL contribute nothing and SHALL NOT be an error — a drained dirty set routinely names a chunk that has since been emptied, and that is exactly the key whose geometry a host must drop. Its reported range SHALL be empty and SHALL sit where the key's geometry would have begun, so a host can address it without a special case.

Ranges SHALL be reported in the order the keys were given.

The whole-grid mesh SHALL be unchanged by the existence of this call: same signature, same merge across chunk boundaries, byte-identical vertex and index buffers, so export keeps the tighter merge.

The cost SHALL follow the chunks named and not the grid: meshing k of a grid's n occupied chunks SHALL cost about k/n of meshing it whole.

#### Scenario: A per-chunk mesh describes the same surface as a whole one
- **WHEN** a grid is meshed whole and then meshed again by naming every occupied chunk
- **THEN** the two describe the same surface — the same set of exposed unit faces with the same colours, so the same total covered area — and the per-chunk mesh has no fewer triangles than the whole one

#### Scenario: A quad merged across a chunk seam splits rather than cracks
- **WHEN** a slab of material spanning a chunk boundary is meshed whole and then per chunk
- **THEN** the whole mesh merges the face into one quad, the per-chunk mesh emits one quad per chunk, and the two cover exactly the same area with no gap and no overlap

#### Scenario: An unrequested neighbour still hides a face
- **WHEN** two chunks hold material touching across their shared face and only one of them is named
- **THEN** the named chunk emits no quads on that face, exactly as the whole-grid mesh does not

#### Scenario: An emptied chunk meshes to an empty range
- **WHEN** a key naming a chunk the grid no longer holds is meshed
- **THEN** the call succeeds, contributes no geometry, and reports a zero-length range for that key

#### Scenario: The ranges partition the mesh
- **WHEN** several chunks are meshed in one call
- **THEN** the ranges are contiguous, in the order requested, cover the whole output exactly once, and no triangle in one key's index range references a vertex in another key's vertex range

#### Scenario: An incremental sequence matches a from-scratch mesh
- **WHEN** a stroke is applied to a sculpt, the dirty chunks are drained and meshed, and the result is patched over the chunks' previous geometry
- **THEN** the patched surface equals a whole-grid mesh of the same grid, face for face and colour for colour

#### Scenario: A dab costs the dab
- **WHEN** one brush dab on a realistic sculpt dirties a handful of a grid's occupied chunks and those chunks are re-meshed
- **THEN** the work is a small multiple of one chunk's cost rather than the whole grid's, and the ratio to the whole-grid re-mesh is about the ratio of the counts

### Requirement: A voxel grid meshes to quads in two modes
`VoxelGrid` SHALL provide a quad mesher offering two modes, because a voxel sculpt is two different subjects depending on what the user made.

**Dual mode** SHALL be the lattice dual over the same occupancy field `mesh_smooth` builds — the rounded form, quads meeting four to a vertex on average — generalised to a lattice cell size other than the grid's voxel size by sampling that occupancy TRILINEARLY.

At the grid's own voxel size with no blur the sampler reads exactly the values `mesh_smooth` reads, so dual mode SHALL return `mesh_smooth`'s mesh vertex for vertex and index for index, differing only by the quad array. This identity is what keeps the two on one code path rather than two that drift.

A cell size COARSER than a voxel low-passes the occupancy and CAN drop a one-voxel-thick feature entirely — the same failure `blur` already carries, for the same reason, and stated in the same place. A cell size FINER than a voxel resamples the same step field: it adds quads and no detail, so the count search SHALL clamp there and report that it clamped.

**Faces mode** SHALL emit one planar, axis-aligned quad per exposed voxel face — the greedy sweep with merging switched off. It SHALL NOT change `mesh_greedy`, whose merged output and its existing per-chunk guarantees are untouched.

Faces mode SHALL WELD its corners, keyed by lattice corner AND palette index. Today's `emit_quad` pushes four fresh vertices per face, so a greedy mesh arrives in a DCC as disconnected rectangles; welding makes it a connected quad grid within a colour region, and splits it at a colour boundary so per-face palette colour survives — which is why those vertices were duplicated in the first place.

Faces mode SHALL NOT emit vertex normals. A welded corner is shared by faces pointing three ways and has no single normal; averaging would round a cube and duplicating would undo the weld. The quads are planar, so a consumer derives the face normal from the face. A caller who needs per-face normals uses `mesh_greedy` and gets triangles, as today.

Faces mode has no cell size — its lattice is the grid. Its count lever SHALL be the multi-resolution LEVEL the grid already carries, so its granularity is roughly a factor of four per step, and a requested target SHALL pick the nearest level. This SHALL be stated, because a caller who asks for fifty thousand quads and receives twelve thousand needs to know a level was chosen rather than a bug hit.

The faces search SHALL be a WALK of that stack and not a search over a cell size that is then rounded to a level: it meshes the coarsest level first, stops at the first level whose count reaches or passes the target, and returns the nearer of the two levels it landed between. The count rises with every level, so no later level can be nearer. The walk SHALL mesh each level at most once, and it starts at the coarsest, so it costs one mesh for every level up to and including the one that stops it: a target met at level k costs k+1 meshes, which is what `iterations` SHALL report. A caller budgeting for this SHALL be told to price the stack's length and not the bracketing pair, because the levels below the bracket were meshed to reach it. `max_iterations` SHALL NOT apply to it: a stack is its own bound, and because each level holds about a quarter of the next one's faces, walking the whole stack costs about a third more than meshing its finest level alone — which a target above every level buys in any case.

With NO target the count search SHALL be the plain quad mesher with a report attached, in both modes and for every input the plain mesher takes. A level the grid does not have SHALL therefore come back as an EMPTY mesh and a zeroed report, as the plain mesher already answers it, rather than be clamped onto the finest level — clamping would hand a caller who passed a stale level a full-resolution mesh and a report naming a level they did not ask for.

`clamped` in faces mode SHALL mean THE STACK RAN OUT — the target is below what the COARSEST LEVEL THAT YIELDS ANYTHING gives, or above what the finest yields. The qualifier is normative, not prose: the stack is not a strict mip, a sculpt made only at a fine level leaves the coarse levels EMPTY, and an empty level meshes to zero quads, which is below every target without being a level the caller can be handed. Reading the coarse end off level 0 would therefore report a grid whose every usable level overshoots as if it bracketed the target. A level whose count EQUALS the target SHALL NOT be reported as clamped — nothing overshot and nothing ran out. A target falling BETWEEN two levels SHALL NOT be reported as clamped even when the level returned is far from it; that is what `within_tolerance` false says. The documented meaning of the flag and the search that sets it SHALL be the same thing, because this feature's premise is that a caller learns the behaviour from the documentation rather than from the mesh.

#### Scenario: A faces target inside the stack brackets it and is not clamped
- **WHEN** a multi-level grid is asked in faces mode for a count between two levels' counts
- **THEN** both bracketing levels are meshed, the nearer one is returned and named in the report, and `clamped` is false
- **AND** `iterations` is the stopping level's index plus one, counting every level meshed from the coarsest, rather than the two levels of the bracket

#### Scenario: A faces target outside the stack reports the stack running out
- **WHEN** a multi-level grid is asked in faces mode for a count below what its coarsest level yields, or above what its finest level yields
- **THEN** the end level is returned and `clamped` is true, and a stack longer than `max_iterations` still reaches its end

#### Scenario: An empty coarse level is not the coarse end of the stack
- **WHEN** a grid sculpted only at a fine level — its coarse levels empty — is asked in faces mode for a count below what its first non-empty level yields
- **THEN** that level's mesh is returned and `clamped` is true, because no level of this grid is nearer
- **AND** the same grid asked for exactly that level's count reports `clamped` false and `within_tolerance` true

#### Scenario: An untargeted fit at a level the grid does not have is empty
- **WHEN** a grid is quad-meshed with no target at a level index beyond its stack, in either mode
- **THEN** the mesh is empty and the report is zeroed, matching what the plain quad mesher returns for the same options

#### Scenario: Dual mode at the grid's own resolution is the smooth mesh
- **WHEN** a sculpt is quad-meshed in dual mode at the grid's voxel size with no blur
- **THEN** the positions and triangle indices are identical to `mesh_smooth`'s, and the mesh additionally carries one quad per two triangles

#### Scenario: Faces mode is one quad per exposed face
- **WHEN** a solid block of voxels is quad-meshed in faces mode
- **THEN** the output carries exactly one quad per exposed voxel face, every quad is planar and axis-aligned, and the quads cover exactly the surface `mesh_greedy` covers over the same cells

#### Scenario: Faces mode welds within a colour and splits across one
- **WHEN** a two-colour slab is quad-meshed in faces mode
- **THEN** faces of the same palette index sharing a lattice corner share one vertex, and the corner on the boundary between two palette indices appears once per colour

#### Scenario: Greedy meshing is untouched
- **WHEN** `mesh_greedy` and `mesh_greedy_chunks` run after faces mode exists
- **THEN** their output is identical, vertex for vertex and index for index, to what they produced before it

#### Scenario: A cell finer than a voxel is clamped
- **WHEN** dual mode is asked for a cell size below the grid's voxel size
- **THEN** it meshes at the voxel size and reports that it clamped, rather than spending the quads on detail the grid does not hold

### Requirement: A grid meshes smoothly as well as blockily
A grid SHALL offer a smooth surface mesh beside `mesh_greedy`, so a sculpt can be displayed as a form rather than as boxes. Greedy meshing emits axis-aligned quads by construction, which is correct for hard-surface work and for export and is the wrong picture of an organic sculpt.

The smooth mesher SHALL place one vertex per surface cell at the centroid of that cell's edge crossings, over occupancy sampled at voxel centres. The centroid is what does the smoothing: a corner cell's vertex is pulled toward the average of its crossings, so the corner rounds without any filtering step. No occupied cell SHALL disappear as a consequence of smoothing at the default setting — a lone voxel has a sign change on each of its six edges and SHALL still produce a surface.

An optional occupancy blur MAY be offered for a smoother result and SHALL NOT be the default, because a blur is what erases thin features: an isolated voxel under a 3x3x3 tent sits near 0.3 occupancy, below the isolevel, and vanishes. A caller asking for it is asking for that trade.

The documentation SHALL state which setting an organic sculpt wants, because the default is not it: the unblurred surface rounds corners but still terraces, since every crossing over binary occupancy interpolates to the same midpoint. One blur pass is what reads as clay. A default that silently deletes thin detail is the wrong default however good it looks, so the recommendation belongs in prose rather than in the default.

Vertex colour SHALL be the average of the palette colours of the occupied cells adjacent to that vertex. A smooth surface has no per-quad facet to carry one palette entry, so two colours meeting gradate across a cell rather than meeting on a line.

`mesh_greedy` SHALL be unchanged in behaviour and output. It remains what export uses and what a hard-surface voxel sculpt wants, and the smooth mesher SHALL NOT be described as replacing it.

The smooth mesher SHALL be documented as a PREVIEW path and SHALL NOT claim manifold or watertight output, for the reason `mesh/surface_nets.h` already gives: a cell crossed twice by the surface receives one vertex and the sheets pinch.

#### Scenario: A cube sculpt rounds rather than staying a cube
- **WHEN** a solid box of voxels is meshed smoothly
- **THEN** its corners are rounded, its faces are not planar quads spanning the box, and the same grid meshed with `mesh_greedy` still returns the flat-faced box unchanged

#### Scenario: A lone voxel survives
- **WHEN** a grid holding one occupied cell is meshed smoothly at the default setting
- **THEN** it produces a closed surface around that cell rather than an empty mesh

#### Scenario: Colour blends across a boundary rather than facetting
- **WHEN** two adjacent regions of different palette indices are meshed smoothly
- **THEN** vertices between them carry a blend of the two palette colours, and no vertex carries a colour absent from the palette

#### Scenario: The blocky mesh is untouched
- **WHEN** any grid is meshed with `mesh_greedy` before and after this change
- **THEN** the vertex and index buffers are byte-identical

### Requirement: A grid converts to a field without a mesh in between
A grid SHALL convert directly to a sampled field, so a sculpt can re-enter the document as an operand. The existing route — mesh the grid, sample the triangles — resamples twice, builds a BVH to do it, and drops the palette, and SHALL NOT be the only way back.

The conversion SHALL locate the surface by reading occupancy with trilinear interpolation between cell CENTRES rather than as cells, so the isosurface is a surface and not a staircase. Nothing SHALL be filtered by default, so no occupied cell disappears; an optional blur MAY be offered and SHALL be documented as costing thin features.

The result SHALL be redistanced, so its stored values are the distance to their own zero set and it carries a Lipschitz bound. A field whose values are an occupancy ramp crosses zero in the right place and says nothing truthful about distance, and every marcher and blend downstream would be working from a guess.

The conversion SHALL accept ONE palette index, converting only the cells carrying it. A field has nowhere to store a palette, so converting per entry is what lets colour survive: the parts placed together are the whole solid and the interface between two colours is interior to their union.

The conversion SHALL NOT modify the grid.

The conversion SHALL be documented as LOSSY IN BOTH DIRECTIONS. Going to voxels quantises to the lattice and no care on the way back recovers a boolean's sharp edge; coming back turns binary occupancy into a distance. What is preserved is the surface within about a cell and the colour; what is not is exactness and the procedural history.

#### Scenario: A sculpt makes the return trip
- **WHEN** a document is blocked out with booleans, rasterized into a grid, sculpted with the voxel verbs, and converted back
- **THEN** the result is an operand: it can be placed in a layer and booleaned against, and it evaluates solid where the sculpt is and carved where a subtraction was placed

#### Scenario: The surface comes back within a cell
- **WHEN** a ball of voxels is converted and the field is sampled where the ball's surface is, on and off axis
- **THEN** the field reads within about a cell of zero at every such point

#### Scenario: The field measures distance, not occupancy
- **WHEN** a converted field is sampled deep inside the solid
- **THEN** the value is approximately the distance to the surface rather than a fraction of an occupancy step

#### Scenario: One palette entry converts alone
- **WHEN** a grid carrying two palette entries is converted once per entry
- **THEN** each result is solid only where its own entry's cells are, and an entry the grid does not carry converts to nothing

### Requirement: The SDF-to-voxel direction states what it guarantees
The direction that already worked SHALL say what it preserves, because a round trip is only as trustworthy as the half nobody wrote down. `rasterize_tape` sets the cells whose CENTRE evaluates inside the tape, over the world region it is given, and colours each from the tape's colour field through the nearest palette entry, adding entries as needed.

What that guarantees, and what it does not, SHALL be stated rather than inferred:

- **The surface moves by up to half a cell.** Membership is decided at the cell centre, so a surface passing through a cell includes or excludes the whole cell. This is the quantisation the return trip cannot undo and it is the reason the round-trip tolerance is a cell rather than zero.
- **A feature thinner than a cell may vanish entirely.** Nothing samples between centres, so a wall, a rib or a gap narrower than the lattice can fall between them and leave no cells at all. Rasterizing finer is the only answer; the return trip cannot invent what was never stored.
- **A sharp edge becomes a staircase**, at the cell size, in the lattice's axes. It comes back from a conversion rounded rather than sharp, and no care on the return recovers it — the information is gone at this step, not the next one.
- **Colour is quantised to the palette**, by nearest entry, and entries are added as needed up to the palette's limit. Two colours closer than the palette's tolerance become one.
- **The region bounds the work.** Content outside the region given is not rasterized and is not reported as missing; a caller that passes a region smaller than the document silently voxelises part of it.

These are properties of sampling a continuous field onto a lattice rather than defects, and a caller SHALL be able to read them without measuring them.

#### Scenario: The quantisation is stated where a caller reads it
- **WHEN** a caller looks up what rasterizing a document into a grid preserves
- **THEN** the half-cell surface movement, the loss of sub-cell features, the staircased edge, the palette quantisation and the region bound are all documented, rather than being discoverable only by experiment

### Requirement: Rasterizing a triangle mesh
`VoxelGrid` SHALL rasterize a triangle mesh directly into cells, in ONE sampling, without an intermediate field or document.

Membership SHALL be decided at the cell centre by the GENERALIZED WINDING NUMBER, the same sign the mesh-to-field import uses. A mesh with a hole, a flipped normal or a self-intersection SHALL rasterize without inverting a half-space: the sign degrades continuously across an opening rather than flipping, which a ray-parity or nearest-triangle-normal test cannot do.

The region SHALL be OPTIONAL and SHALL default to the mesh's own bounds. A document can be unbounded and a mesh cannot, so the default exists here where it does not for a tape. An explicit region SHALL bound the work, and content outside it SHALL be neither rasterized nor reported as missing.

Colour SHALL come from the mesh's vertex colours where it carries them, read at the closest point on the nearest triangle and quantised to the palette by nearest entry. A mesh carrying no colours SHALL take a single neutral entry rather than a colour invented per cell.

The grid's existing contracts SHALL hold unchanged: the change counter SHALL move only for cells whose value actually changed, so rasterizing the same mesh twice SHALL report no change the second time.

An empty mesh, an empty or infinite region, and a mesh whose every index is out of range SHALL each leave the grid untouched and SHALL NOT be errors.

#### Scenario: The solid the triangles bound is filled
- **WHEN** a closed mesh is rasterized
- **THEN** cells inside it are occupied, cells outside are not, and the occupied volume matches the solid's to within the surface's own half-cell

#### Scenario: A hole degrades the sign rather than inverting it
- **WHEN** a mesh missing one face is rasterized
- **THEN** the interior is still occupied, the exterior is still empty, and most of the solid survives

#### Scenario: One sampling keeps what two lose
- **WHEN** a feature thinner than two cells is rasterized directly and through the field-then-document detour
- **THEN** the direct path occupies at least as many cells, and on a thick shape the two agree to within about a cell of surface

#### Scenario: Colour survives the direct path
- **WHEN** a mesh carrying vertex colours is rasterized
- **THEN** the palette carries the mesh's colours, where the same model through the detour arrives with none because a distance field carries no colour

#### Scenario: Rasterizing twice changes nothing the second time
- **WHEN** the same mesh is rasterized into the same grid twice
- **THEN** the change counter moves on the first call and not on the second

### Requirement: The BVH names the triangle nearest a point
`mesh::Bvh` SHALL answer a closest-point query with the point on the surface, the source triangle carrying it, and that point's barycentric coordinates — so an attribute on the mesh can be read where the query landed.

The unsigned-distance query SHALL be defined in terms of it, so the two cannot disagree about which point is nearest, and its results SHALL be unchanged.

A tie between two equally near triangles SHALL resolve to the one found first, so the answer does not depend on the order the tree happened to be traversed in.

#### Scenario: An attribute can be read off the nearest triangle
- **WHEN** the closest point to a query is on a triangle whose corners carry different colours
- **THEN** the returned barycentrics reconstruct the point from those corners, and interpolating the corner colours by them gives the colour at that point

#### Scenario: Distances did not move
- **WHEN** the same unsigned-distance and winding queries are run before and after this change
- **THEN** the results are identical

### Requirement: A level may be refined over part of the lattice
A resolution level SHALL be able to carry explicit storage for only part of the lattice. Where a level has no storage for a cell, the level's value at that cell SHALL be its PARENT's value, resolved up the chain to level 0 — not "empty".

This SHALL be a storage change and not a semantic one. Every cell of every level SHALL still have a value, so integer cell coordinates, O(1) neighbour indexing and the meshing of any level SHALL be unchanged. In particular it SHALL NOT go near the integer-coordinate hash that makes a soft stroke reproducible across platforms, and SHALL NOT reopen the choice of discrete levels over adaptive refinement.

Refinement SHALL be tracked at CHUNK granularity, the storage and indexing unit the grid already has. A caller's region SHALL be rounded OUT to the chunks it touches; tracking an arbitrary box would let a single write near a boundary re-materialise everything in between.

A level SHALL distinguish a chunk that is refined and empty from a chunk that is not refined. Without that distinction an empty region and an unrefined one read alike, and the unrefined one would report its parent's material as absent.

Adding a level with NO region SHALL remain exactly what it is today: every chunk refined, the whole lattice. Existing callers and existing documents SHALL see no change, and the serialised form of such a grid SHALL be byte-identical to what it was.

#### Scenario: An unrefined chunk reads its parent
- **WHEN** a level is refined over one region and a cell outside it is read
- **THEN** the value is the parent level's value at the containing coarse cell, not zero

#### Scenario: Refining a region does not move the solid
- **WHEN** a level is added over a region of an occupied grid
- **THEN** the surface the grid represents is unchanged at every level

#### Scenario: A region costs its region
- **WHEN** a level is added over a small region rather than the whole lattice
- **THEN** its stored cell count is proportional to the region, not to the occupied volume

#### Scenario: Whole-lattice refinement is untouched
- **WHEN** a level is added with no region
- **THEN** the grid stores, reads and serialises exactly as it did before regions existed

### Requirement: Writing outside a refined region refines it
A write to a cell in an unrefined chunk SHALL refine that chunk, seeded from its parent so the solid does not move, and then apply the write.

Refusing the write would break any brush whose footprint straddles a boundary, which is the common case rather than the exceptional one. Refining on demand also makes the stored set track what was actually TOUCHED rather than what was reserved.

Refinement SHALL be upward-closed: refining a chunk SHALL materialise its ancestors, because propagating a fine edit downward needs somewhere coarse to write it.

#### Scenario: A brush straddling the boundary works
- **WHEN** a brush footprint spans refined and unrefined chunks
- **THEN** every cell in the footprint is written, and the chunks it reached are now refined

#### Scenario: Seeding preserves the solid
- **WHEN** an unrefined chunk is refined by a write
- **THEN** every cell in it other than those the write touched reads as it did before

### Requirement: The refined set survives the document
The serialised voxel stream SHALL record which chunks each level has refined, tagged so that a reader predating regions still opens the grid rather than failing on it.

A grid whose levels are all wholly refined SHALL serialise as it did before this existed, so the change costs nothing for the documents that do not use it.

#### Scenario: A regionally refined grid round-trips
- **WHEN** a grid with a partially refined level is saved and loaded
- **THEN** every cell of every level reads as it did, and the refined set is the same

#### Scenario: The common case is byte-identical
- **WHEN** a grid with no partially refined level is serialised
- **THEN** the bytes are the same as before regions existed

### Requirement: The occupied extent is computed once per edit, not once per ray
`bounds_min` and `bounds_max` SHALL share one cached walk per level, invalidated by the write path.

They walk every material cell, and the voxel raycast asks for BOTH on every ray — so rendering a grid paid two full walks per pixel. On a 65k-cell sculpt that was the whole of a 29 second render, and a partially refined level, whose inherited chunks cost more to read, made it eight times worse. Caching removes the per-ray cost rather than making the walk cheaper, because the walk is only wrong to repeat, not wrong.

The cache SHALL be invalidated for the level written AND for every level above it, since a level above inherits from the one below and a write there can move its extent.

Reading an inherited chunk SHALL resolve its ancestor chunk ONCE and index that chunk's data directly, rather than calling the per-cell accessor, which would hash and recurse per cell.

Caching makes the two accessors lazily mutating, so concurrent first calls on one grid race. This SHALL be documented where the cache lives rather than left implicit, because a `const` method otherwise carries the expectation that concurrent readers are safe.

#### Scenario: The extent follows an edit that shrinks it
- **WHEN** the cell at the extreme of a grid's extent is erased
- **THEN** the reported extent pulls back in, rather than keeping the erased cell

#### Scenario: An edit at one level moves another level's extent
- **WHEN** a broad stroke is made at the coarse level
- **THEN** the finer level's reported extent reflects it

#### Scenario: A partially refined level reports the extent of the solid
- **WHEN** a level refined over a region is asked for its bounds
- **THEN** they are the same bounds the wholly refined level reports

### Requirement: A grid can carry more than one resolution
A voxel grid SHALL be able to hold a stack of levels, each with half the cell size of the one below it, and SHALL expose adding a level, dropping one, and choosing which is active.

A grid with a single level SHALL behave exactly as a grid does today, so that existing documents and existing calls are unaffected.

Adding a level SHALL subdivide, splitting every occupied cell into its eight children, so the solid the grid represents is exactly the same solid rather than a resample of it. Dropping a level SHALL remove the finest, along with the detail only that level held.

#### Scenario: A single-level grid is unchanged
- **WHEN** a grid is built without ever adding a level
- **THEN** every verb, the serialised bytes and the meshed result are identical to the same grid before this change

#### Scenario: Adding a level does not move the surface
- **WHEN** a level is added to a grid
- **THEN** the surface the grid represents is unchanged within the finer level's cell size

### Requirement: Detail survives a change of level
Moving to a coarser level SHALL average — a coarse cell is occupied when at least half its eight children are, and takes the commonest child's colour — and moving back SHALL restore the finer detail rather than the averaged approximation, so that working on big forms at a coarse level does not destroy small ones already sculpted.

This is the property the whole feature exists for: without it, stepping down a level is a destructive operation and the workflow it is meant to enable does not work.

Each level above the coarsest SHALL therefore hold, alongside its cells, the OFFSETS that distinguish it from the level below: exactly the cells whose value differs from the cell above them. An edit at any level SHALL average down into the coarser levels and replay into the finer ones from their offsets, so the offsets are the authority wherever they exist and derived values fill in everywhere else. An offset the coarser form has caught up with SHALL be dropped, so the record stays the size of the detail rather than the size of the level.

A consequence follows and is intended: a coarse edit SHALL NOT be able to erase detail held at a finer level, because an offset is not something a coarser level can address. Removing that detail is done at the level it lives on.

#### Scenario: Fine detail survives a coarse edit
- **WHEN** detail is sculpted at the finest level, the active level is dropped, a broad stroke is made, and the finest level is made active again
- **THEN** the broad stroke is present and the fine detail is still there

#### Scenario: A trip through another level changes nothing on its own
- **WHEN** the active level is changed and changed back with no edit in between
- **THEN** every level holds exactly the cells it held before

### Requirement: A verb states its level
Every sculpting and repair verb SHALL act on a stated level. The level SHALL be carried by the footprint walk they share rather than added to each verb's arguments, so that a verb added later cannot omit it and no existing signature has to change to gain one.

The stated level is the grid's ACTIVE level, and every cell-addressed query — occupancy, bounds, flood select, picking, the SDF bridges — SHALL read the same one, so that "which level am I on" has a single answer rather than one per entry point.

#### Scenario: A verb acts only on its level
- **WHEN** a verb runs against a chosen level
- **THEN** the other levels are unchanged except as the detail-preservation rule requires

#### Scenario: A mask selects the same region at every level
- **WHEN** the same world-addressed mask gates the same verb at two different levels
- **THEN** the same world-space region is protected at both, because the walk converts a cell to world space through the active level's cell size

### Requirement: The dither stays reproducible per level
A falloff brush resolves sub-unit strength by hashing the cell coordinate, and that hash is what makes a stroke reproducible across platforms and backends. Each level SHALL be a uniform lattice with its own cell coordinates, so the hash keeps that guarantee at every level.

#### Scenario: The same stroke at the same level gives the same cells
- **WHEN** the same soft stroke is applied twice to the same level on any platform or backend
- **THEN** exactly the same cells are set

### Requirement: A painted mask can gate an SDF operation
A painted `MaskField` SHALL be convertible into the form an SDF item's gate consumes, so the mask an artist paints protects both representations rather than only the voxel one.

The two SHALL agree: a region protected from a voxel edit SHALL be protected from the equivalent SDF operation.

#### Scenario: The same painted mask protects both representations
- **WHEN** the same mask gates a voxel edit and the equivalent SDF operation
- **THEN** the protected region is unchanged in both

### Requirement: A voxel drag is a gesture, not a sequence of calls
A voxel grab applied repeatedly SHALL be expressible as a transaction that captures the material as it was when the gesture began and resamples from that capture, so that the result depends on the TOTAL displacement and not on how the host chose to deliver it.

A stateless grab SHALL NOT be expected to compose. Each call reads the grid and writes back, so a second call reads the first's output; and the displacement is rounded to whole cells after the falloff weights it, so a finely split drag can move nothing at all while every call succeeds. That SHALL be stated where the stateless call is documented.

An update SHALL take the total displacement from the anchor and SHALL be idempotent: repeating one changes nothing, and returning to the anchor restores the material.

A single update from a fresh transaction SHALL produce cell-for-cell what the stateless call produces for the same displacement.

The captured region MAY widen as the drag reaches further, which is sound because a grab writes only inside its own footprint: any cell the capture later takes in has not been written by the gesture.

Cancelling SHALL restore the material exactly, and abandoning a transaction without committing SHALL cancel rather than leave a partial gesture in the grid.

#### Scenario: The split does not change the result
- **WHEN** the same total drag is delivered as one, two, four and eight updates
- **THEN** every one of them leaves the grid in the same state

#### Scenario: A split drag still moves material
- **WHEN** a drag of several cells is delivered in eight updates
- **THEN** material occupies cells it did not occupy before

#### Scenario: An update is idempotent
- **WHEN** the same total displacement is passed to update more than once
- **THEN** the grid is what one update left

#### Scenario: A drag returns to where it started
- **WHEN** a drag is updated out and then back to zero
- **THEN** the material is what it was when the gesture began

#### Scenario: One update is the stateless call
- **WHEN** a fresh transaction is updated once
- **THEN** the grid matches what the stateless grab produces for that displacement

#### Scenario: An abandoned drag leaves nothing behind
- **WHEN** a transaction is destroyed without being committed
- **THEN** the material is what it was when the gesture began
