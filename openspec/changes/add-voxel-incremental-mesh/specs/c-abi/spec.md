# c-abi — an incremental voxel display path

Delta for `add-voxel-incremental-mesh` (#86, part 2).

## ADDED Requirements

### Requirement: A host can display a voxel sculpt incrementally
The ABI SHALL expose the grid's dirty-chunk drain and its regional mesh, so that a host can display a sculpt by re-meshing what changed rather than the whole grid. The two calls SHALL follow the brick cache's refill vocabulary rather than inventing a parallel one: drain the keys, mesh the keys, patch the ranges.

The drain SHALL take a capacity and report a count and a remainder, exactly as the brick-cache drain does, and SHALL NOT accept a NULL buffer as a size query — the two shapes are already distinguished in this header and mixing them is what makes a retry loop ambiguous. A host SHALL be able to call it in a loop until the remainder is zero, or to stop early and leave the rest queued for the next frame. Keys SHALL cross as packed `int32` triples, the same spelling `clay_voxel_flood_select` and `clay_brick_cache_surface_bricks` use.

The regional mesh SHALL take that key list and SHALL fill, when asked, one range record per key in the order given, carrying the key and its vertex and index ranges — an array ELEMENT with a fixed layout, not a versioned descriptor, for the same reason `clay_brick_mesh_range` is one. The header SHALL state that these ranges PARTITION the mesh with no vertex shared between keys, which is the difference from the brick ranges and the property that lets a host free one key's slice without consulting its neighbours. It SHALL state why: a voxel face belongs to one cell in one chunk, so clamping the merge to a chunk boundary splits quads instead of cracking the surface, and there are no straddlers to attribute.

Asking for ranges without a key list SHALL be refused, as the brick mesh refuses it and for the same reason: with no key list there is no count the caller could have sized the buffer from.

A key naming a chunk the grid does not hold SHALL contribute an empty range and SHALL NOT be an error, because a drained set routinely names a chunk that has since been emptied and that is precisely the key whose geometry the host must drop.

`clay_voxel_mesh` SHALL keep meaning "mesh the whole grid" and SHALL be unchanged by this addition — same signature, same output byte for byte — so export and a first full display keep the merge that spans chunk boundaries. The addition SHALL be purely additive: no existing signature changes, no struct grows, no enumerator's value changes.

Both calls SHALL act on the grid's ACTIVE level, as every other cell-addressed call in this header does, and the header SHALL say so, because the dirty set is per level.

#### Scenario: A host displays a stroke without re-meshing the model
- **WHEN** a host rasterizes a sculpt, meshes it whole once, then applies a dab, drains the dirty chunks and meshes exactly those keys
- **THEN** it receives one range per key, and patching those ranges over the previous per-chunk geometry yields the same surface a whole-grid re-mesh describes

#### Scenario: The drain is capacity-in, count-out
- **WHEN** the drain is called with a buffer smaller than the number of dirty chunks
- **THEN** it writes what fits, reports how many it wrote and how many remain, and a loop that keeps calling until the remainder is zero receives every key exactly once

#### Scenario: A NULL buffer is not a size query
- **WHEN** the drain is called with a NULL key buffer
- **THEN** it is refused as an invalid argument rather than reporting a required size

#### Scenario: Ranges require a key list
- **WHEN** the regional mesh is asked for ranges with no keys
- **THEN** it is refused rather than inferring a count from the grid's current chunk set

#### Scenario: A stale key is an ordinary key
- **WHEN** a key drained before the chunk was emptied is passed to the regional mesh
- **THEN** the call succeeds and reports a zero-length range for it

#### Scenario: The whole-grid call did not move
- **WHEN** the same grids are meshed by `clay_voxel_mesh` before and after this change
- **THEN** the vertex count, the index count and a hash of every attribute and index buffer are identical
