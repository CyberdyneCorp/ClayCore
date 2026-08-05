# build-packaging Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
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

### Requirement: Module dependency rule
Module layering SHALL be enforced (by include checks in CI): `kernel` depends on nothing; `scene`/`brick`/`mesh`/`voxel`/`pick` depend only on `kernel`+`math`; backends depend on `eval`; `io` and bindings sit on top; no module depends on a backend.

#### Scenario: Layering violation fails CI
- **WHEN** a source file in `scene/` includes a backend header
- **THEN** the include-graph check fails the build naming the offending edge

### Requirement: Dependency policy
Third-party dependencies SHALL be permissively licensed only (MIT/BSD/zlib/Apache-2.0): ufbx, meshoptimizer, metal-cpp, nanobind, xsimd, optionally cgltf/tinyply, doctest/Catch2 + benchmark. Boost, GPL/LGPL code, and exceptions across the ABI SHALL NOT be introduced. assimp SHALL be used only in CI as an independent validator, never linked into the shipping library. A license manifest SHALL be generated and checked in CI.

#### Scenario: License gate
- **WHEN** a new dependency with a non-permissive license enters the build
- **THEN** the CI license check fails

### Requirement: clay-cli
The repository SHALL ship `clay-cli` with at minimum: `clay mesh in.clayspace --res N -o out.{obj,fbx,ply,glb}`, `clay validate mesh-or-document`, `clay eval --points pts.npy`, and `clay convert` between supported formats — exercising only public library APIs.

#### Scenario: Headless meshing
- **WHEN** `clay mesh scene.clayspace --res 512 -o out.fbx` runs on a Linux CI runner
- **THEN** it produces a watertight FBX and exits 0, or exits non-zero with a diagnostic

### Requirement: Test pyramid
CI SHALL run, in order: kernel unit tests against reference values from `docs/01-sdf-math-foundations.md`; property tests (Lipschitz bounds hold, blends rigid, locality bit-identity); the backend parity suite per registered backend; golden-scene meshing gates (watertight/manifold across the op matrix); I/O round-trip and fuzz tests; and performance benchmarks (points/sec, bricks/sec, mesh time on fixed scenes) with regression gates.

#### Scenario: Perf regression gate
- **WHEN** a change slows a benchmark scene beyond the configured regression threshold
- **THEN** CI fails with the benchmark name, baseline, and measured value

### Requirement: Versioning
The C ABI and Python API SHALL follow SemVer; kernel headers may evolve freely within a major. The document format version is independent (backward-open, forward-refuse per `file-io`). GPU backend availability SHALL never change results — only speed — enforced by the parity suite as a release gate.

#### Scenario: Release checklist enforced
- **WHEN** a release tag is cut
- **THEN** CI verifies ABI version bump correctness (no symbol/layout break on minor), wheel builds, and parity-suite pass on all registered backends

### Requirement: Examples CI job
CI SHALL build the Python module and run every script under `examples/`, failing the build on a non-zero exit from any of them.

#### Scenario: Examples job runs the gallery
- **WHEN** the examples job runs on a pull request
- **THEN** every example executes against the freshly built wheel and the job fails if any script raises

