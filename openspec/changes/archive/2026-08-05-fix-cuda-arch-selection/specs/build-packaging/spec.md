# build-packaging — the CUDA preset configures against any GPU

Delta for `fix-cuda-arch-selection`.

## MODIFIED Requirements

### Requirement: CMake presets and platform matrix
The library SHALL build with CMake presets `cpu-only` (macOS/Linux/Windows), `+metal` (Apple), `+cuda`, and `+opencl`, with warnings-as-errors everywhere and ASan/UBSan jobs in CI. The core SHALL be headless: no UI, windowing, or Apple frameworks in `include/clay/` or `src/` (Apple dependencies are confined to `backends/metal/` and packaging).

The `+cuda` preset SHALL configure whenever a CUDA toolkit is present, however new the installed GPU is. When the detected GPU architecture is one the toolkit can target, the build SHALL target it directly. When it is not — a GPU newer than the toolkit — the build SHALL target the newest architecture the toolkit supports as PTX only, so the driver JIT-compiles for the actual device, and SHALL report both architectures at configure time. An explicit `CMAKE_CUDA_ARCHITECTURES` SHALL override the selection. The selected architecture SHALL be applied to the `claycore` target itself, since the target is created before the CUDA language is enabled and no longer inherits the variable.

#### Scenario: Three-OS headless build
- **WHEN** CI builds `cpu-only` on macOS, Linux, and Windows runners
- **THEN** the library and full test suite compile and pass on all three

#### Scenario: GPU newer than the CUDA toolkit
- **WHEN** `cmake --preset cuda` runs on a machine whose GPU architecture the installed nvcc cannot emit a cubin for
- **THEN** configuration succeeds with a PTX-only build of the newest supported architecture, reports the substitution, and the CUDA backend registers and passes the parity suite on that GPU

#### Scenario: Explicit architecture wins
- **WHEN** the build is configured with `-DCMAKE_CUDA_ARCHITECTURES=<arch>`
- **THEN** that value is used unchanged and no fallback is applied
