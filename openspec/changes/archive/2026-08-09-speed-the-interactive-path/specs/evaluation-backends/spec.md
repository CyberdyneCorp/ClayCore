# evaluation-backends — batch dispatch balances across unequal cores

Delta for `speed-the-interactive-path`.

## ADDED Requirements

### Requirement: Batch dispatch load-balances rather than partitioning once
The CPU backend's parallel dispatch SHALL divide a batch into more chunks than it has workers, so that a worker finishing early claims further work instead of idling. Handing each worker exactly one chunk makes every call cost as much as its slowest chunk, and the cores a batch runs on are not interchangeable: a mobile SoC pairs fast cores with efficiency cores by design, and the operating system may be running something else on any of them.

The minimum chunk size a caller asks for SHALL still be honoured, so a batch too small to divide is unaffected and no chunk becomes smaller than the cost of claiming it.

#### Scenario: A batch is divided into more chunks than there are workers
- **WHEN** a batch large enough to exceed the caller's minimum chunk size on every worker is dispatched
- **THEN** it is divided into several chunks per worker

#### Scenario: A small batch is unaffected
- **WHEN** a batch smaller than the caller's minimum chunk size times the worker count is dispatched
- **THEN** the chunk size is the caller's minimum, exactly as before

### Requirement: Every element of a batch is computed exactly once
Whatever chunking a dispatch chooses, it SHALL cover the batch exactly: no element left uncomputed and none computed twice. A chunking error of either kind produces a wrong value rather than a crash, and only at sizes that happen to divide badly.

#### Scenario: Coverage holds across sizes that straddle the chunking boundaries
- **WHEN** point evaluation is run at sizes either side of the minimum chunk size, at exact multiples of the chunk count, and at sizes that divide evenly into nothing
- **THEN** every result equals the scalar reference for that point, and no output element is left at its initial value

#### Scenario: Results do not depend on the chunking
- **WHEN** the same batch is evaluated by the threaded path and by the scalar reference
- **THEN** the values are identical
