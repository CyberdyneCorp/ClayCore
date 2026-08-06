# sdf-kernels — kernel headers as a host artifact

Delta for `add-host-kernel-package`.

## ADDED Requirements

### Requirement: Kernel headers self-select their backend
When no `CLAY_KERNEL_*` macro is defined, `shim.h` SHALL choose the backend
branch from the compiling toolchain's own predefined macros — `__METAL_VERSION__`
for MSL, `__CUDACC__` for CUDA device code, `__OPENCL_VERSION__` /
`__OPENCL_C_VERSION__` for OpenCL C — and fall back to the CPU branch only when
none is present. An explicitly defined `CLAY_KERNEL_*` SHALL always win, so
existing builds and the host-emulated CI profiles are unaffected.

This is what lets a host compile the headers as shader source with no build
settings: including them from a `.metal` file is enough.

#### Scenario: A .metal file needs no build flags
- **WHEN** a host `#include`s `clay/kernel/kernels.h` from Metal shader source without defining any `CLAY_KERNEL_*` macro
- **THEN** the MSL branch of the shim is selected and the file compiles

#### Scenario: An explicit selection still wins
- **WHEN** a translation unit defines `CLAY_KERNEL_CUDA` and is compiled by a host C++ compiler
- **THEN** the CUDA branch is selected regardless of what the compiler predefines

### Requirement: Umbrella header
The kernel module SHALL provide `clay/kernel/kernels.h`, a single header that
includes the whole dialect in dependency order, so a consumer needs one include
and does not have to track the file list. It SHALL omit the headers a backend
cannot compile — `field.h` is templated and therefore not part of the OpenCL C
subset — rather than failing on them.

#### Scenario: One include reaches the whole vocabulary
- **WHEN** a translation unit includes only `clay/kernel/kernels.h`
- **THEN** every primitive, operator, deformer, lift, easing curve and the tape interpreter are available

### Requirement: Kernel headers are consumable by a host shader compiler
The kernel headers SHALL be publishable as a standalone artifact — no build
step, no generated file, no dependency on anything outside
`include/clay/kernel/` — and SHALL be verified to compile as MSL in CI: with
`xcrun metal` where an Apple toolchain exists, and against a stubbed
`metal_stdlib` elsewhere, so a break in the Metal branch of the shim fails a
Linux runner rather than waiting for an Apple one.

#### Scenario: A Metal-only break fails Linux CI
- **WHEN** a kernel header uses a helper that the Metal branch of `shim.h` does not define
- **THEN** the kernel-dialect check fails on a Linux runner naming the header and the missing symbol

#### Scenario: The artifact stands alone
- **WHEN** the packaged headers are copied into an unrelated project and compiled as MSL
- **THEN** they compile without any other file from this repository
