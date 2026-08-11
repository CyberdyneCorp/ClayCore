# brick-cache — the cache carries colour and pads its readback

Delta for `close-webgpu-host-abi-gaps`.

## ADDED Requirements

### Requirement: Optional per-brick colour
The cache SHALL be able to carry a colour lattice alongside each surface brick's distance lattice, in the same lattice dimensions, produced by the same evaluation that produces the distances and submitted through the same call.

Colour SHALL be opt-in per cache and chosen at creation, because a lattice that was never evaluated cannot be produced on demand: a cache created without colour SHALL behave exactly as a cache with no colour concept, and asking it for colour SHALL be refused rather than answered with a default.

Colour payload SHALL count against the cache's memory budget on the same footing as distance payload, so the configured ceiling continues to bound what a brick costs rather than part of it.

A brick that is uniformly inside or outside SHALL allocate no colour lattice, and SHALL still answer a colour readback with a single value repeated across the lattice, so that a consumer uploading brick payloads never branches on state.

The level-1 mip SHALL carry no colour lattice, and a colour readback at that level SHALL be refused rather than answered by a filtering policy the cache would be choosing on the consumer's behalf.

#### Scenario: Colour survives the refill path
- **WHEN** a colour-carrying cache is marked dirty, drained, evaluated for distance and colour, and submitted
- **THEN** the accepted bricks read back a colour lattice whose values are those the field carries at the corresponding lattice points

#### Scenario: A distance-only cache stays a distance-only cache
- **WHEN** a cache is created without colour
- **THEN** it stores no colour, its memory usage is unchanged from before colour existed, and a colour readback against it is refused

#### Scenario: A uniform brick still answers
- **WHEN** a brick that is uniformly inside or outside is read back with colour
- **THEN** its whole colour slice is written with one value, allocating nothing

#### Scenario: Colour is bounded by the budget
- **WHEN** a colour-carrying cache is filled against a memory budget
- **THEN** the budget is never breached, and the accounting includes the colour payload

### Requirement: Brick readback can be padded from neighbours
Brick readback SHALL accept an optional apron: a request to write each brick padded by a chosen number of voxels on every face, taken from the neighbouring bricks, so that a consumer uploading a brick as a texture tile can filter across the brick boundary without fetching neighbours itself.

The padded form SHALL keep the fixed-stride contract: every brick occupies the same number of elements whatever its state, so the destination remains a buffer a consumer uploads without a packing pass.

Halo samples SHALL be defined for every neighbour, including bricks that are implicit and bricks the cache has never evaluated, using the same band values a single-sample read of those bricks reports. There SHALL be no coordinate in the halo for which the cache has no answer.

A key the cache holds nothing for SHALL leave its whole padded slice untouched, exactly as it does unpadded: the rule is about the key, not about its neighbourhood.

An apron wider than the brick SHALL be refused rather than clamped, on the same reasoning that refuses a level-of-detail above the one that exists — silently answering a differently-sized tile puts wrongly-shaped data in a texture.

#### Scenario: A padded tile filters correctly at a seam
- **WHEN** two adjacent surface bricks are read back with a one-voxel apron
- **THEN** each tile's halo holds its neighbour's boundary samples, so trilinear interpolation across the shared face agrees with a sample taken from the cache directly

#### Scenario: The edge of the sculpted region is defined
- **WHEN** a surface brick at the edge of the tracked region is read back with an apron
- **THEN** its halo holds the band values its absent or implicit neighbours report, and no sample is undefined

#### Scenario: The stride stays fixed under padding
- **WHEN** a mixed set of surface, uniform and missing keys is read back with an apron
- **THEN** each key occupies the same padded stride, and only the missing key's slice is left unwritten

#### Scenario: An over-wide apron is refused
- **WHEN** a readback asks for an apron wider than the brick's own lattice
- **THEN** the call is refused and nothing is written

### Requirement: A subset of bricks can be meshed
Meshing the cache SHALL accept an explicit set of brick keys as well as the whole surface set, so that a consumer holding a dirty key list can re-mesh what changed rather than what exists. Naming no set SHALL continue to mean every surface brick.

A key in the set that stores no lattice SHALL contribute nothing and SHALL NOT be an error, because a drained dirty set routinely contains bricks that turned out uniform.

The triangles produced for a brick's cells SHALL be those the whole-surface mesh produces for the same cells; a subset SHALL differ only in that a vertex shared with a cell outside the subset is emitted independently, never in that a cell is skipped or a crossing is missed.

Meshing SHALL be able to report, per key in the order given, the contiguous vertex and index ranges that key contributed, so that a consumer can write into sub-ranges of a buffer instead of rebuilding it.

#### Scenario: A dab costs the dab
- **WHEN** a small edit dirties a handful of bricks and only those keys are meshed
- **THEN** the work is proportional to those bricks rather than to the surface, and the triangles produced match the corresponding triangles of a whole-surface mesh

#### Scenario: A uniform key in the set is ordinary
- **WHEN** a key list contains bricks that are uniformly inside or outside
- **THEN** they contribute no triangles and the call succeeds

#### Scenario: Ranges partition the output
- **WHEN** per-key ranges are requested for a key list
- **THEN** the reported vertex ranges and index ranges are contiguous, non-overlapping, and together cover the whole mesh

### Requirement: Brick raycasting has a batched form
Raycasting the cached bricks SHALL have a batched form taking many rays in one call, in the same packed ray layout and with the same optional outputs as the document-level batched raycast, so a consumer writes one call shape for both surfaces.

The batched form SHALL start no thread, consistent with the cache owning none, and SHALL report a ray that hits nothing as an ordinary result rather than as a failure.

#### Scenario: A batch agrees with the single-ray path
- **WHEN** a set of rays is cast in one batched call and the same rays are cast one at a time
- **THEN** the hits, distances, positions and normals are identical
