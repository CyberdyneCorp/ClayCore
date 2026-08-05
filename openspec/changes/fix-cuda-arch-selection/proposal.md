# Proposal: the `cuda` preset configures on a GPU newer than the toolkit

## Why

`cmake --preset cuda` fails outright on an RTX 5060 (sm_120) with CUDA 12.0:

```
CMake Error in CMakeLists.txt:
  CUDA_ARCHITECTURES is empty for target "claycore".
```

Two independent faults produce it.

`CMAKE_CUDA_ARCHITECTURES` was set to `native`, which expands to the installed
GPU's architecture. When the GPU is newer than the toolkit, nvcc cannot emit a
cubin for it, CMake drops the unsupported value, and the list ends up empty.
The message names no architecture and no toolkit, so the failure reads as a
broken checkout rather than a version mismatch.

The assignment could not have worked in any case: `claycore` is created near
the top of the file, long before `enable_language(CUDA)` runs, and a target's
`CUDA_ARCHITECTURES` property is initialized from the variable *at target
creation time*. Setting the variable afterwards is a no-op for that target,
so even a valid list would not have reached the compile line.

The effect is that the CUDA backend cannot be built at all on current hardware
— the tier-2 backend and its parity gate are unreachable on exactly the GPUs
it targets.

## What Changes

- **Architecture selection falls back instead of failing.** If the detected
  GPU architecture is in the toolkit's supported set it is used as-is.
  Otherwise the build takes the newest architecture the toolkit does know and
  emits PTX only (`-virtual`, no cubin), which the driver JITs for the actual
  device at load. A `STATUS` message names both architectures so the fallback
  is visible rather than silent.
- **The choice is applied to the target, not the variable**, so it survives
  `claycore` having been created before the CUDA language was enabled.
- **An explicit `-DCMAKE_CUDA_ARCHITECTURES=...` still wins.** It is captured
  before `enable_language(CUDA)`, which is the only point where a user-supplied
  value is distinguishable from CMake's own default.
- **The selection moves into `cmake/ClayCudaArch.cmake`** as a pure function of
  (native arch, supported list), so it can be tested without a CUDA toolkit or
  a GPU. `tests/cmake/test_cuda_arch.cmake` is the regression test and runs as
  the `clay_cuda_arch_selection` CTest case on every preset.

## Capabilities

### Modified Capabilities

- `build-packaging`: the `+cuda` preset SHALL configure whenever a CUDA
  toolkit is present, regardless of how new the installed GPU is.

## Impact

- `CMakeLists.txt`, new `cmake/ClayCudaArch.cmake`, new
  `tests/cmake/test_cuda_arch.cmake`, `tests/CMakeLists.txt`, `README.md`.
- No runtime or API impact: this is a build-configuration fix. Parity results
  are unchanged — a PTX-JIT build of the CUDA backend passes the same suite as
  a cubin build.
- The new CTest case needs no CUDA and runs on `cpu-only` too, so the
  regression is caught on runners without a GPU.
