# brick-cache Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Sparse brick storage
`clay::brick` SHALL store the evaluated field as a sparse virtual grid of bricks (8³ or 16³, configurable per document resolution) holding fp16 distance values in a narrow band of ±3 voxels around the surface. Bricks entirely inside or outside SHALL be represented implicitly (sign-only), not allocated.

#### Scenario: Empty space costs nothing
- **WHEN** a small object sits in a large layer volume
- **THEN** only surface-crossing bricks are allocated; interior/exterior bricks consume no per-voxel storage

### Requirement: Dirty tracking and incremental re-evaluation
The cache SHALL maintain a dirty set derived from edit influence bounds: a mutation marks exactly the bricks its influence bound intersects. Re-evaluation SHALL process only dirty bricks, using per-brick culled tapes from the scene module.

#### Scenario: Local edit, local work
- **WHEN** an item with a small influence bound is modified in a large scene
- **THEN** only bricks intersecting that bound are marked dirty and re-evaluated; all other bricks are untouched (bit-identical, per the locality guarantee)

### Requirement: Async-friendly plain-data requests
Brick evaluation requests SHALL be plain data (brick id lists + tapes) with no callbacks or hidden threading; the consumer (app or CLI) owns queues and scheduling through the backend interface. The cache SHALL be safely usable with evaluation in flight: results for stale requests SHALL be identifiable and discardable.

#### Scenario: Stale result discarded
- **WHEN** a brick is re-dirtied while an evaluation request for it is in flight and the old result arrives
- **THEN** the cache detects the stale generation and does not overwrite the newer dirty state

### Requirement: LOD mip bricks
The cache SHALL maintain lower-resolution mip bricks for far-view use, derived from full-resolution brick data, and expose which LOD is current per region.

#### Scenario: Mip consistency
- **WHEN** full-resolution bricks for a region are up to date
- **THEN** requesting that region's mip brick returns data downsampled from those bricks (not from stale data)

### Requirement: Deterministic memory ceiling
The cache SHALL enforce a configurable memory budget suitable for mobile: allocation beyond the budget SHALL fail predictably (error code / eviction policy), never abort or throw across the ABI, and the current usage SHALL be queryable.

#### Scenario: Budget exceeded
- **WHEN** an evaluation would allocate bricks beyond the configured budget
- **THEN** the request returns a budget-exceeded result identifying the shortfall, and existing brick data remains valid

### Requirement: The cache is reachable from the C ABI
The brick cache SHALL be reachable through the C ABI, since the C ABI is the only surface a packaged consumer has. The exposed surface SHALL mirror `brick::BrickCache` — an opaque handle created from a versioned configuration descriptor, plus dirty marking, a request drain, submission, brick readback, surface enumeration, statistics, LOD mips, meshing and raycasting — and SHALL NOT drive evaluation itself: the consumer still owns queues, threading and scheduling, so no refill loop, thread pool, time budget or ordering policy is published.

Evaluation requests SHALL cross the boundary as a fixed-layout array element that is byte-for-byte `brick::BrickRequest`, so a drain is a copy rather than a transcription, and the two layouts SHALL be pinned by static assertion.

Because the C ABI must produce a per-brick culled tape, the boundary SHALL also expose dense-grid evaluation with an optional cull region, and the influence bound an edit dirties (per node and per layer, reporting the unbounded case rather than claiming a finite box for it).

Evaluation across the boundary SHALL reach the named backend as batched work (the evaluation-backends batched grid form), not as one backend call per brick, so a GPU backend can amortize its per-submission overhead over the batch. The values SHALL be those of the per-brick culled tapes regardless of how the batch is submitted.

#### Scenario: A packaged consumer refills incrementally
- **WHEN** a host holding only the C header marks an edit's influence bound dirty, drains the requests, evaluates them and submits the results
- **THEN** only the bricks the bound reached are re-evaluated, and every other brick's stored payload is bit-identical

#### Scenario: A region no cache could hold is refused, not attempted
- **WHEN** a dirty region spanning more bricks than the batch ceiling, or reaching a brick coordinate outside `int32`, crosses the boundary
- **THEN** the call is refused with an invalid-argument error and the cache is left exactly as it was, rather than converting the region and allocating from it

#### Scenario: A stale submission is an outcome, not a failure
- **WHEN** a brick is re-dirtied while a request for it is in flight and the old result is submitted
- **THEN** the call succeeds and reports the submission as stale through its result out-parameter

### Requirement: A level can be sampled and enumerated, not only read back
The cache SHALL answer a decoded sample AT A LEVEL, and SHALL enumerate the keys a level stores. Before this the cache could sample and enumerate the full-resolution level only, and a mip was reachable through whole-brick readback alone — which is why nothing but a readback consumer could use one.

A level that holds no brick for a key SHALL answer the outside band value, as a never-evaluated brick does, so that a lattice walk over any level is total and a consumer needs no special case at the edge of the built region. The level-0 answers SHALL be exactly what the existing sample and enumeration return.

The count of built mips SHALL be available in constant time, so a consumer can ask whether a level exists at all without enumerating it.

#### Scenario: A level's enumeration is what it stores
- **WHEN** some coarse blocks have their mips built and others do not
- **THEN** enumerating level 1 yields exactly the coarse keys whose mip is valid, and enumerating a level above 1 yields nothing

#### Scenario: Sampling outside the built region is defined
- **WHEN** a level is sampled at a key it holds no brick for
- **THEN** the answer is the outside band value rather than an error or an undefined read

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

A subset SHALL also return the straddlers: every whole-surface triangle with at least one corner inside a requested brick's closed box whose cell is owned by an unrequested brick, each attributed to the lexicographically lowest (x, then y, then z) requested key owning one of its corners. Without them a subset omits triangles no request can name, and no sequence of subset calls can maintain a complete surface — the consumer is forced back onto the whole-surface re-mesh the key list exists to avoid. A subset SHALL NOT contain any triangle the whole-surface mesh does not contain.

Meshing SHALL be able to report, per key in the order given, the contiguous vertex and index ranges that key contributed — its own cells' triangles first, then its attributed straddlers — so that a consumer can write into sub-ranges of a buffer instead of rebuilding it.

#### Scenario: A dab costs the dab
- **WHEN** a small edit dirties a handful of bricks and only those keys are meshed
- **THEN** the work is proportional to those bricks rather than to the surface, and the triangles produced match the corresponding triangles of a whole-surface mesh

#### Scenario: A dab's subset carries its boundary
- **WHEN** an edit's dirty keys are meshed as a subset
- **THEN** every whole-surface triangle with at least one corner inside those bricks is present — the straddlers included — and nothing else is

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

### Requirement: The cache states which level it holds
The brick cache is a sparse narrow band around one lattice, and that lattice is the one its `BrickConfig` names. It SHALL be unaffected by a voxel grid's resolution levels: it caches SDF evaluation, never voxel occupancy, so adding, dropping or selecting a voxel level SHALL neither dirty a brick nor invalidate a mip.

A voxel level and a brick mip are separate mechanisms for separate representations, and neither SHALL be implemented in terms of the other.

#### Scenario: A level change does not drop the cache
- **WHEN** a voxel grid's active level changes
- **THEN** every tracked brick keeps its generation and nothing is resubmitted

