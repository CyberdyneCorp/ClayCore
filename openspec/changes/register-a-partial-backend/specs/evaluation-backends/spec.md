# evaluation-backends — register a partial backend

Delta for `register-a-partial-backend` (#63, second half).

## MODIFIED Requirements

### Requirement: Runtime backend registry
Backends SHALL be runtime-registered. The CPU backend SHALL be compiled in unconditionally on every platform. GPU backends (Metal, CUDA, OpenCL) SHALL register only when their platform/runtime is available, and their absence SHALL never change results — only performance.

A backend SHALL register when its CORE operations are available, rather than requiring every operation it could provide. The core operations are point evaluation and grid evaluation: a backend that cannot do those accelerates nothing and SHALL NOT register. An operation the backend cannot run SHALL report `false` from `caps()` and SHALL return `Status::Unsupported` when called, which is the refusal the caller already handles for on-device meshing.

A partial backend SHALL register under its ORDINARY name. A host asks for `metal` to get acceleration, and per-operation refusal is how this interface already says "not from me"; a distinct name would force every host to match on a string it has never seen, which fails harder than the `Unsupported` it must already handle.

Batched operations SHALL NOT be core. Where the base class provides a loop over the single-item operation with identical results — `eval_grid_batch` over `eval_grid`, `eval_points_batch` over `eval_points` — a backend whose batch path is unavailable SHALL fall back to that loop rather than refuse. Such a fallback costs submission overhead and nothing else, so it SHALL NOT be reported as an unsupported operation.

A backend that is compiled in and did NOT register SHALL record why, in a form a caller can read back. A registry that answers with the same list for "this build has no Metal" and "this machine's Metal was discarded" gives a host no way to tell a missing feature from a broken one.

#### Scenario: CPU always present
- **WHEN** claycore is built with the `cpu-only` preset on any supported platform
- **THEN** the registry contains the CPU backend and the full test suite passes

#### Scenario: Missing GPU degrades gracefully
- **WHEN** a document is evaluated on a machine without any GPU backend
- **THEN** evaluation completes on CPU with results identical (within parity tolerance: exact, since CPU is the reference) to a GPU-equipped run

#### Scenario: One unavailable pipeline does not delete the backend
- **WHEN** a GPU backend's runtime builds its point and grid pipelines but fails to build its raycast pipeline
- **THEN** the backend registers, `caps()` reports raycast as unavailable, `raycast()` returns `Status::Unsupported`, and point and grid evaluation run on that backend with results identical to the CPU reference

#### Scenario: An unavailable batch path costs speed, not capability
- **WHEN** a GPU backend cannot build its batched grid pipeline but can build the single-grid one
- **THEN** `eval_grid_batch` returns the same values it would have returned, by looping over `eval_grid`, and reports itself as available

#### Scenario: A discarded backend says why
- **WHEN** a backend is compiled in, attempts to initialize, and fails
- **THEN** the reason is recorded and readable through the registry, and is distinguishable from the backend never having been compiled in
