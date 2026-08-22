# evaluation-backends

## ADDED Requirements

### Requirement: A batch of grids is one dispatch, not one per grid
A backend's batched grid evaluation SHALL be dispatched as a single unit of parallel work over the whole batch, rather than as one dispatch per grid.

This is what a brick refill is: a dab dirties a dozen or so bricks and they are evaluated together. Looping the single-grid path over them costs a dispatch-and-join barrier per brick, and — because a brick is only eight cells across — bounds each of those barriers to eight threads however many the machine has. Measured before this requirement was met: 6.7 of 16 physical cores, with the share FALLING as the document grew.

The unit of parallel work SHALL be small enough that a batch offers the pool substantially more units than the machine has threads. A grid's z-slices are not, for a brick.

#### Scenario: A refill occupies the machine
- **WHEN** a dab's worth of bricks is evaluated as one batch on a machine with many cores
- **THEN** the work is spread across them rather than bounded by the number of slices in one brick

### Requirement: Batched and single-grid evaluation are the same numbers
A backend's batched grid evaluation SHALL produce, for each grid in the batch, exactly what its single-grid evaluation produces for that grid — bit for bit on a CPU backend, and within the backend's stated tolerance on a device backend.

A batch is an optimisation of HOW the work is dispatched and never of what is computed. A brick refill and a one-off evaluation of the same region are the same question, and a host that meshes from one and picks against the other would see a seam where they disagreed. Bit-identity is required of the CPU backend rather than a tolerance because it is available: every sample is the same tape evaluated at the same point, and nothing accumulates across samples.

#### Scenario: The batch matches the grids it batches
- **WHEN** a batch of grids is evaluated, and then each of those grids is evaluated on its own
- **THEN** every distance and every colour matches exactly on the CPU backend
