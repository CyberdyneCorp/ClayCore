# brick-cache — the cache reaches the consumer

Delta for `expose-the-brick-cache`.

## ADDED Requirements

### Requirement: The cache is reachable from the C ABI
The brick cache SHALL be reachable through the C ABI, since the C ABI is the only surface a packaged consumer has. The exposed surface SHALL mirror `brick::BrickCache` — an opaque handle created from a versioned configuration descriptor, plus dirty marking, a request drain, submission, brick readback, surface enumeration, statistics, LOD mips, meshing and raycasting — and SHALL NOT drive evaluation itself: the consumer still owns queues, threading and scheduling, so no refill loop, thread pool, time budget or ordering policy is published.

Evaluation requests SHALL cross the boundary as a fixed-layout array element that is byte-for-byte `brick::BrickRequest`, so a drain is a copy rather than a transcription, and the two layouts SHALL be pinned by static assertion.

Because the C ABI must produce a per-brick culled tape, the boundary SHALL also expose dense-grid evaluation with an optional cull region, and the influence bound an edit dirties (per node and per layer, reporting the unbounded case rather than claiming a finite box for it).

#### Scenario: A packaged consumer refills incrementally
- **WHEN** a host holding only the C header marks an edit's influence bound dirty, drains the requests, evaluates them and submits the results
- **THEN** only the bricks the bound reached are re-evaluated, and every other brick's stored payload is bit-identical

#### Scenario: A region no cache could hold is refused, not attempted
- **WHEN** a dirty region spanning more bricks than the batch ceiling, or reaching a brick coordinate outside `int32`, crosses the boundary
- **THEN** the call is refused with an invalid-argument error and the cache is left exactly as it was, rather than converting the region and allocating from it

#### Scenario: A stale submission is an outcome, not a failure
- **WHEN** a brick is re-dirtied while a request for it is in flight and the old result is submitted
- **THEN** the call succeeds and reports the submission as stale through its result out-parameter
