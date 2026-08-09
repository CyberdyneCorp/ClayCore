# c-abi — the cache reaches the consumer

Delta for `expose-the-brick-cache`.

## ADDED Requirements

### Requirement: The brick cache across the C ABI
The ABI SHALL expose the brick cache as an opaque handle the caller creates from a versioned configuration descriptor and destroys, never bound to a document, alongside the three calls that make it usable: dense-grid evaluation with an optional cull region, and the influence bound of a node and of a layer.

There SHALL be exactly one refill path — mark dirty, drain requests, evaluate, submit — with the drain taking a capacity and reporting a count and a remainder rather than accepting a NULL buffer as a size query, and with the outcome of a submission (accepted, stale, over budget) crossing as an out-parameter with a success return, on the same footing as "nothing to undo".

The handle SHALL take no lock and start no thread; the header SHALL state that calls on one handle are the host's to serialize and that the batched evaluation call, which takes no handle, is free-threaded against one const document.

A dirty region SHALL be validated in 64-bit before the engine converts it: a non-finite or empty region, a brick coordinate outside `int32`, and a span above the batch ceiling SHALL each be refused with the cache left unchanged. Dirtying everything the cache tracks SHALL be spelled as the absence of a region, never as a region carrying an infinity.

#### Scenario: A host refills from the header alone
- **WHEN** a consumer with only `clay.h` marks a layer's influence bound dirty, drains the requests in fixed-size chunks, evaluates them and submits the values
- **THEN** every brick is accepted, the pending count reaches zero, and the surface bricks read back at a fixed stride in the engine's own fp16 bits

#### Scenario: A count is never inferred
- **WHEN** a value buffer is passed whose length is not exactly the request count times the brick's sample count
- **THEN** the call is refused rather than reading or writing what the caller did not allocate

#### Scenario: A request carries everything its evaluation needs
- **WHEN** a request is drained and evaluated
- **THEN** the lattice AND the band come with it, so the evaluator culls against the brick dilated by the band without consulting the cache, and there is no value a caller can supply wrongly

#### Scenario: The bound to dirty is not the bound to frame on
- **WHEN** a consumer asks for a node's influence bound
- **THEN** it receives a box no tighter than the layer bounds query reports, and an explicit flag for the items whose influence is unbounded
