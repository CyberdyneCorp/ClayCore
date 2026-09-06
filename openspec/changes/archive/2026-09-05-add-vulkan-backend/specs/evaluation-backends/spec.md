# evaluation-backends — a Vulkan compute backend in the empty slot

Delta for `add-vulkan-backend`.

## ADDED Requirements

### Requirement: Vulkan backend (tier 3)
The Vulkan backend SHALL run on Vulkan compute, from a stated minimum API version and a stated extension set, and SHALL implement at minimum `eval_points` and the grid evaluation that fills bricks. Capabilities it does not provide — raycast, whose sphere-tracing utilities are templated C++ that a compute shader dialect cannot compile, and device meshing where it is not implemented — SHALL report `Unsupported` so callers fall back to another backend.

It SHALL pass the parity suite where registered, at the same tolerance as every other GPU backend. Its absence on any platform SHALL NOT block any other capability.

Its shaders SHALL be derived from the existing single-source kernels rather than transcribed from them. If a new dialect profile is required, it SHALL be added to the dialect check that gates every push, so that a kernel change that breaks it fails in seconds rather than at release.

It SHALL be built with a resident tape and reused buffers rather than uploading the tape and allocating buffers per dispatch — a cost already identified on another backend and not worth re-creating here.

This backend is not part of the Apple production path: on Apple hardware Vulkan is a translation layer over Metal and cannot outperform the Metal backend it translates into.

#### Scenario: Registration and fallback
- **WHEN** the Vulkan backend is present and a caller requests a capability it does not implement
- **THEN** it returns `Unsupported` and the caller can fall back to another registered backend

#### Scenario: Parity where registered
- **WHEN** the parity suite runs against a registered Vulkan backend on a real device
- **THEN** every kernel agrees with the CPU scalar reference within its documented tolerance

#### Scenario: A tape is uploaded once per change
- **WHEN** a request batch is evaluated against one unchanged document
- **THEN** the tape is uploaded once for the batch

#### Scenario: Absence blocks nothing
- **WHEN** no Vulkan runtime is present
- **THEN** every other backend and capability behaves exactly as before

### Requirement: A software runtime gates plumbing, not arithmetic
Where the parity suite is run against a SOFTWARE Vulkan implementation, the result SHALL be described as gating the plumbing — SPIR-V validity, descriptor and buffer layout, dispatch and readback — and SHALL NOT be presented as evidence that the backend's arithmetic matches the CPU reference. A software runtime executes on the CPU, so agreement with the CPU is close to guaranteed by construction.

Device parity SHALL remain a hardware-dependent check, named as such alongside the other checks that require hardware.

#### Scenario: A software-runtime job says what it proves
- **WHEN** a CI job runs the Vulkan backend against a software runtime
- **THEN** its name and its documentation state that it gates plumbing rather than arithmetic

#### Scenario: Device parity is a release check
- **WHEN** a release touches the kernels
- **THEN** Vulkan device parity is listed among the manual hardware-dependent checks to run
