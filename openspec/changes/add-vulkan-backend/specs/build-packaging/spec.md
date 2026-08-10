# build-packaging — a Vulkan preset and an honest CI job

Delta for `add-vulkan-backend`.

## ADDED Requirements

### Requirement: Vulkan is a build option, never a requirement
The build SHALL offer a Vulkan preset alongside the existing backend presets, and the backend SHALL be off by default. A build without a Vulkan SDK or runtime SHALL configure, compile and test exactly as it does today.

Whatever shader form the backend consumes SHALL be produced by the build from the single-source kernels, not committed as generated output that can drift from the source it was generated from.

If the shader route introduces a new dialect profile, `check_kernel_dialect.py` SHALL compile the kernel headers under it on every push, as it already does for the CPU, CUDA and Metal profiles and the OpenCL amalgamation.

#### Scenario: A build without Vulkan
- **WHEN** the project is configured on a machine with no Vulkan SDK
- **THEN** configuration, build and the full test suite behave exactly as before

#### Scenario: Shaders are generated, not committed
- **WHEN** the Vulkan backend is built
- **THEN** its shader binaries are produced from the kernel headers during the build

#### Scenario: A dialect break fails fast
- **WHEN** a kernel header changes in a way the Vulkan shader route cannot compile
- **THEN** the per-push dialect check fails, naming the header, without requiring a Vulkan device
