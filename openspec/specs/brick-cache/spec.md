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

