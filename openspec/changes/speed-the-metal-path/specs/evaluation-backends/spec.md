# evaluation-backends — a dispatch costs a dispatch, not an upload

Delta for `speed-the-metal-path`.

## MODIFIED Requirements

### Requirement: Metal backend (tier 1)
The Metal backend SHALL use `metal-cpp` (pure C++, no Objective-C in the core), compile the kernel headers as MSL, pass tapes via argument buffers, and implement the full backend interface including `eval_bricks`. It is the iPad app's production path.

Whether it meshes on the device or triangulates on the host SHALL be reported truthfully by `BackendCaps::device_meshing`, and the two SHALL agree: a capability flag that disagrees with the implementation is a defect, not a detail.

Gradients SHALL be computed on the device. Falling back to the CPU reference evaluator for a whole batch is not an implementation of the GPU path.

#### Scenario: Metal brick fill
- **WHEN** a dirty brick set is submitted to the Metal backend
- **THEN** narrow-band fp16 brick data is returned matching CPU reference within parity tolerance

#### Scenario: The capability flag matches the implementation
- **WHEN** `caps().device_meshing` is read
- **THEN** it is true exactly when `mesh()` triangulates on the device

#### Scenario: Gradients do not fall back
- **WHEN** a batch is evaluated on the Metal backend with gradients requested
- **THEN** the gradients are produced on the device and agree with the CPU reference within parity tolerance

## ADDED Requirements

### Requirement: A resident tape is not re-uploaded
A GPU backend SHALL upload a tape's instructions, parameters and blob when that tape changes, and SHALL reuse the resident copy when it does not. Dispatching N times against one unchanged tape SHALL cost one upload, not N.

The residency key SHALL identify the tape unambiguously. A key that can collide across different tapes is a wrong field, so a cheap-but-approximate identity is not acceptable, and a key expensive enough to cost what the upload cost is not a saving.

#### Scenario: A dab uploads one tape
- **WHEN** a brick request batch is evaluated against one unchanged document on a GPU backend
- **THEN** the tape is uploaded once for the batch

#### Scenario: An edit is uploaded
- **WHEN** the document is edited and evaluated again
- **THEN** the dispatch uses the edited tape, and the results are identical to a run with no residency at all

### Requirement: Per-call device allocation is bounded
A GPU backend SHALL NOT allocate and free its input, output and tape buffers on every call. Buffers SHALL be reused across calls, growing to the largest request seen and released when the backend is destroyed.

Where the platform's memory is unified, a backend SHALL avoid copying data it can reference in place, and SHALL keep a copying path for callers whose memory cannot be referenced.

#### Scenario: A steady stream of dispatches stops allocating
- **WHEN** the same-sized query is dispatched repeatedly
- **THEN** device buffer allocations occur on the first calls and not on the later ones

#### Scenario: Results are unchanged by the buffer strategy
- **WHEN** the same query is evaluated with buffer reuse and with a fresh allocation per call
- **THEN** the results are bit-identical
