# voxel-engine — speed the voxel mesh sweep

Delta for `speed-the-voxel-mesh-sweep` (#86, part 1).

## MODIFIED Requirements

### Requirement: Sparse operations cost the material, not its bounding box
Greedy meshing and the mask region operations SHALL cost time and memory in proportion to the cells that carry material, not to the bounding box enclosing them. A grid and a mask are sparse by construction and a bounding box is not: material far apart on two or more axes makes the box enormous while the material stays small.

Neither SHALL size a buffer from a difference of two lattice coordinates, which overflows a 32-bit integer for coordinates a deserialized grid or mask may legitimately carry, and would then ask for an allocation no allocator can satisfy — which ends the process rather than returning, since the library builds without exceptions.

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
