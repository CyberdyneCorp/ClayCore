# evaluation-backends — Backend interface, CPU/Metal/CUDA/OpenCL, parity

Delta for `add-claycore-v1`.

## ADDED Requirements

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

#### Scenario: CPU always present
- **WHEN** claycore is built with the `cpu-only` preset on any supported platform
- **THEN** the registry contains the CPU backend and the full test suite passes

#### Scenario: Missing GPU degrades gracefully
- **WHEN** a document is evaluated on a machine without any GPU backend
- **THEN** evaluation completes on CPU with results identical (within parity tolerance: exact, since CPU is the reference) to a GPU-equipped run

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
The OpenCL backend SHALL target OpenCL 3.0 with kernel headers constrained (via shim macros) to the C-compatible subset, implement at minimum `eval_points` and `eval_bricks`, and pass the parity suite where registered. Its absence on any platform SHALL NOT block any other capability. The backend interface SHALL NOT assume OpenCL specifics, so a future Vulkan-compute backend can slot into the same interface.

#### Scenario: OpenCL optional
- **WHEN** claycore is configured without the `+opencl` preset
- **THEN** the build, tests, and all other backends are unaffected
