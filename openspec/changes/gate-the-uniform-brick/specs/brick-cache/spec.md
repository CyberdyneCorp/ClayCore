# brick-cache — the cache knows where its surface is, and stores what a walk would

Delta for `gate-the-uniform-brick`.

## ADDED Requirements

### Requirement: The cache keeps the bound of its surface bricks
The cache SHALL answer the union of its Surface bricks' boxes — exactly the fold of `brick_bounds` over `surface_bricks()`, and empty when there is no Surface brick — without enumerating them. The brick raycast needs that box before its first step and runs once per Pencil event, and on a whole-model cache enumerating every surface key per ray cost more than the march it preceded.

The bound SHALL be maintained by the mutations, never by the query: a submit that produces a Surface brick widens it, and a Surface brick leaving that state — a submit that reclassifies it uniform, an eviction, a trim — SHALL leave the bound equal to the fold before the mutating call returns, refolding from the map when the brick could have decided a face and doing nothing when it could not. A single trim SHALL refold at most once however many bricks it drops.

A const query SHALL NOT write the bound. The cache is read from many threads at once — the batched brick raycast fans its rays across the worker pool against a const cache — and a lazily refolded field behind a const accessor is a data race whatever value it holds. Every const query stays a pure read; the fold is paid on the mutating side.

#### Scenario: The bound equals the fold after every mutation
- **GIVEN** a cache filled over a sculpt
- **WHEN** an interior brick is evicted, a face brick is evicted, the cache is trimmed with and without a focus, empty bricks are forgotten, a brick is refilled from a shape that reclassifies it uniform and back to surface, every brick is evicted one at a time, and the whole cache is refilled
- **THEN** after each step the bound equals the fold over `surface_bricks()` on all six fields, to exact float equality, and is empty once no Surface brick remains

#### Scenario: A ray does not walk the map
- **WHEN** a ray is cast against a whole-model cache
- **THEN** the domain it marches inside is read from the kept bound, and no per-ray enumeration of the surface bricks takes place

## MODIFIED Requirements

### Requirement: Sparse brick storage
`clay::brick` SHALL store the evaluated field as a sparse virtual grid of bricks (8³ or 16³, configurable per document resolution) holding fp16 distance values in a narrow band of ±3 voxels around the surface. Bricks entirely inside or outside SHALL be represented implicitly (sign-only), not allocated.

The fp16 conversion SHALL be a software round-to-nearest-even, bit-identical on every platform, and SHALL be exact through the subnormal halves: a distance below 2⁻¹⁴ SHALL be stored as the nearest multiple of 2⁻²⁴, with values under 2⁻²⁵ rounding to zero. A sample that close to the surface is ordinary — it is where the surface passes through a lattice point — and storing it as anything but the nearest half poisons the eight cells that interpolate it.

#### Scenario: Empty space costs nothing
- **WHEN** a small object sits in a large layer volume
- **THEN** only surface-crossing bricks are allocated; interior/exterior bricks consume no per-voxel storage

#### Scenario: A distance below 2⁻¹⁴ is stored as the nearest subnormal half
- **WHEN** values across the subnormal range are converted to half and back
- **THEN** each half carries the IEEE round-to-nearest-even bit pattern for its value, and every round trip lies within one subnormal step of its input

### Requirement: The cache is reachable from the C ABI
The brick cache SHALL be reachable through the C ABI, since the C ABI is the only surface a packaged consumer has. The exposed surface SHALL mirror `brick::BrickCache` — an opaque handle created from a versioned configuration descriptor, plus dirty marking, a request drain, submission, brick readback, surface enumeration, statistics, LOD mips, meshing and raycasting — and SHALL NOT drive evaluation itself: the consumer still owns queues, threading and scheduling, so no refill loop, thread pool, time budget or ordering policy is published.

Evaluation requests SHALL cross the boundary as a fixed-layout array element that is byte-for-byte `brick::BrickRequest`, so a drain is a copy rather than a transcription, and the two layouts SHALL be pinned by static assertion.

Because the C ABI must produce a per-brick culled tape, the boundary SHALL also expose dense-grid evaluation with an optional cull region, and the influence bound an edit dirties (per node and per layer, reporting the unbounded case rather than claiming a finite box for it).

Evaluation across the boundary SHALL reach the named backend as batched work (the evaluation-backends batched grid form), not as one backend call per brick, so a GPU backend can amortize its per-submission overhead over the batch. The values SHALL be those of the per-brick culled tapes regardless of how the batch is submitted, with one stated exception: a brick the refill has PROVEN uniform from its culled tape's Lipschitz bound (the c-abi capability states the proof) MAY carry stand-in values — every one beyond the band with the brick's sign, the colour at sample dim^3/2 the field's own — because those are the only properties of a uniform brick's values that submission reads. What the cache STORES for such a brick SHALL be bit-identical to what it stores from the walked samples, so the locality guarantee and the parity fixtures are unchanged by the gate.

#### Scenario: A packaged consumer refills incrementally
- **WHEN** a host holding only the C header marks an edit's influence bound dirty, drains the requests, evaluates them and submits the results
- **THEN** only the bricks the bound reached are re-evaluated, and every other brick's stored payload is bit-identical

#### Scenario: A region no cache could hold is refused, not attempted
- **WHEN** a dirty region spanning more bricks than the batch ceiling, or reaching a brick coordinate outside `int32`, crosses the boundary
- **THEN** the call is refused with an invalid-argument error and the cache is left exactly as it was, rather than converting the region and allocating from it

#### Scenario: A stale submission is an outcome, not a failure
- **WHEN** a brick is re-dirtied while a request for it is in flight and the old result is submitted
- **THEN** the call succeeds and reports the submission as stale through its result out-parameter

#### Scenario: A proven brick stores what a walked one stores
- **WHEN** a whole model is refilled through the boundary with the uniform-brick gate enabled and again with it disabled
- **THEN** every brick's state, halves and colours are identical between the two caches, and the class counts are the same
