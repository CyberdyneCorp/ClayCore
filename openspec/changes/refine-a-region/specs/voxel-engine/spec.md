# voxel-engine — refining a region rather than the whole lattice

Delta for `refine-a-region`.

## ADDED Requirements

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

#### Scenario: The extent follows an edit that shrinks it
- **WHEN** the cell at the extreme of a grid's extent is erased
- **THEN** the reported extent pulls back in, rather than keeping the erased cell

#### Scenario: An edit at one level moves another level's extent
- **WHEN** a broad stroke is made at the coarse level
- **THEN** the finer level's reported extent reflects it

#### Scenario: A partially refined level reports the extent of the solid
- **WHEN** a level refined over a region is asked for its bounds
- **THEN** they are the same bounds the wholly refined level reports
