# voxel-engine — dirty-chunk tracking and a regional mesh

Delta for `add-voxel-incremental-mesh` (#86, part 2).

## ADDED Requirements

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
