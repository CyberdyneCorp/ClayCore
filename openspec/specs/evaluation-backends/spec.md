# evaluation-backends Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Backend interface
`clay::eval::Backend` SHALL define one interface implemented identically by every backend: `eval_points(tape, points[]) → distances[]/gradients[]/colors[]` (batch field queries), `eval_bricks(tape, brick_ids[]) → narrow-band brick data`, `raycast(tape, rays[]) → hits[]`, `mesh(tape | bricks, params) → triangles` (where supported), and capability flags (fp16 storage, on-device meshing, max tape length). All evaluation requests SHALL be plain data (flat buffers) with no per-sample allocation; the caller owns threading and queues.

#### Scenario: Uniform API across backends
- **WHEN** the same tape and point batch are submitted to any two registered backends
- **THEN** both accept the identical request structures and return results in the identical buffer layout

#### Scenario: Capability flags honored
- **WHEN** a backend reports no on-device meshing
- **THEN** `mesh()` on that backend returns a not-supported error code and the caller can fall back to CPU meshing

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

### Requirement: CPU scalar is the correctness reference
The CPU scalar build SHALL define correctness for every kernel and composed scene. The CPU backend SHALL also provide a SIMD batch path (Apple `simd` on Apple platforms, SSE/NEON via xsimd elsewhere) and thread-pool dispatch; the SIMD path SHALL match scalar within 1e-6 relative on distances.

#### Scenario: SIMD parity with scalar
- **WHEN** the parity suite evaluates every kernel on the SIMD path against scalar on the standard sample corpus
- **THEN** all distances agree within 1e-6 relative error

### Requirement: GPU parity tolerance
Every registered GPU backend SHALL match the CPU scalar reference within documented tolerances — default 1e-4 relative on distances, specified per-kernel where tighter or looser — on every kernel and on composed golden scenes. The parity suite SHALL run on every registered backend and SHALL be a CI gate on platforms where that backend exists.

#### Scenario: Per-kernel parity gate
- **WHEN** the parity suite runs a backend against CPU scalar for each kernel over the standard sample corpus
- **THEN** any kernel exceeding its documented tolerance fails CI with the kernel name and worst-case error reported

### Requirement: Batched grid evaluation amortizes device dispatch
The backend interface SHALL accept a batch of same-shape lattices, each carrying its own (typically per-brick culled) tape, as one call (`eval_grid_batch`). Every backend SHALL answer the batch with the values its per-grid path produces; a GPU backend SHALL evaluate the batch in a bounded number of device submissions rather than one submission per lattice, so that per-submission overhead is amortized over the batch instead of multiplying with it. The brick-refill entry point (`clay_brick_cache_eval_requests`) SHALL hand its requests to the named backend through this batched form.

The same batched form SHALL exist for a device-buffer destination (`eval_grid_batch_device`): a batch of same-shape lattices whose results land at fixed per-grid slots in a caller-owned device buffer, honouring each grid's buffer offset exactly as the per-grid device path does. Every backend that serves the per-grid device path SHALL answer the batched device form with identical values (the default runs the per-grid path per lattice); a backend bound to a caller-supplied device SHALL evaluate the batch in a bounded number of device submissions. The device-destination refill entry point (`clay_brick_cache_eval_requests_device`) SHALL hand its requests to the adopted backend through this batched device form, under the same chunking rules as the host-memory form.

#### Scenario: A brick refill is not a round trip per brick
- **WHEN** a batch of brick requests is evaluated through a GPU backend
- **THEN** the batch runs in a bounded number of device submissions, and the refill's cost per brick falls as the batch grows instead of holding at the per-submission overhead

#### Scenario: The device-buffer refill is batched too
- **WHEN** a batch of brick requests is evaluated into a caller-owned device buffer through an adopted GPU backend
- **THEN** the batch runs in a bounded number of device submissions, brick i lands at the same fixed slot the host-memory form uses, and the values are identical to the host-memory batched form on the same backend

#### Scenario: Batched and per-grid evaluation agree
- **WHEN** the same lattices and per-lattice culled tapes are evaluated one call per grid and as one batch, on any registered backend
- **THEN** the batched values match the per-grid values within that backend's parity tolerance, including for a lattice whose culled tape is empty

### Requirement: Repeated evaluation reuses device-resident state
Compiled tapes SHALL carry a content identity (`Tape::compile_id`): a process-unique nonzero id stamped by `compile_document`/`compile_layer`, equal only when the bytes are the same compile's; a tape assembled by hand carries 0 and has no identity. Code that mutates a compiled tape's sections MUST reset the id to 0.

A GPU backend MAY rely on that identity to keep the uploaded form of recently evaluated tapes resident across calls, and SHALL reuse its transfer scratch buffers (grown to the high-water mark) instead of allocating per call — so re-evaluating an unchanged document costs the dispatch, not a re-upload of the tape (notably a consolidated volume's blob) plus buffer allocation. Residency and pooling SHALL change only speed: values are identical to an upload-per-call evaluation, a tape with no identity is re-uploaded per call, and a recompiled document is never served a stale upload — the id is identity, never a size or address heuristic.

#### Scenario: An unchanged document is not re-uploaded per call
- **WHEN** the same compiled tape, carrying a multi-megabyte volume blob, is evaluated repeatedly on a GPU backend
- **THEN** the steady-state cost per call is flat in the blob size, and the values are byte-identical call after call

#### Scenario: A recompiled document is never served stale bytes
- **WHEN** a document is recompiled into a tape whose sections have the same sizes but different bytes, and both tapes are evaluated in turn
- **THEN** each evaluation returns that tape's own field, within the backend's parity tolerance of the scalar reference

### Requirement: Metal backend (tier 1)
The Metal backend SHALL use `metal-cpp` (pure C++, no Objective-C in the core), compile the kernel headers as MSL, pass tapes via argument buffers, and implement the full backend interface including `eval_bricks` and on-device meshing. It is the iPad app's production path.

#### Scenario: Metal brick fill
- **WHEN** a dirty brick set is submitted to the Metal backend
- **THEN** narrow-band fp16 brick data is returned matching CPU reference within parity tolerance

### Requirement: CUDA backend (tier 2)
The CUDA backend SHALL compile the same kernel headers as device code via NVRTC or nvcc and implement at minimum `eval_points`, `eval_bricks`, and `raycast`, with parity enforced. It targets desktop/pipeline/ML workloads (batch evaluation from `pyclay`).

#### Scenario: CUDA batch evaluation from Python
- **WHEN** `pyclay` evaluates 1M points with `backend="cuda"` on a CUDA machine
- **THEN** results match `backend="cpu"` within parity tolerance

### Requirement: OpenCL backend (tier 3, best-effort)
The OpenCL backend SHALL run on OpenCL 3.0 runtimes, built against the CL1.2 common subset the kernels actually use so that 1.2-only implementations also work, with kernel headers constrained (via shim macros) to the C-compatible subset. It SHALL implement at minimum `eval_points` and the grid evaluation that fills bricks, and pass the parity suite where registered. Capabilities it does not provide (raycast, whose sphere-tracing utilities are templated C++ that OpenCL C cannot compile; device meshing) SHALL report `Unsupported` so callers fall back to another backend. Its absence on any platform SHALL NOT block any other capability. The backend interface SHALL NOT assume OpenCL specifics, so a future Vulkan-compute backend can slot into the same interface.

#### Scenario: OpenCL optional
- **WHEN** claycore is configured without the `+opencl` preset
- **THEN** the build, tests, and all other backends are unaffected

#### Scenario: Unsupported capability falls back
- **WHEN** a caller requests `raycast` from the OpenCL backend
- **THEN** it returns `Unsupported` without evaluating, and the caller can obtain identical results from the CPU backend

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

