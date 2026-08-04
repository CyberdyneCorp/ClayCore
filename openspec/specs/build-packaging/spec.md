# build-packaging Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: CMake presets and platform matrix
The library SHALL build with CMake presets `cpu-only` (macOS/Linux/Windows), `+metal` (Apple), `+cuda`, and `+opencl`, with warnings-as-errors everywhere and ASan/UBSan jobs in CI. The core SHALL be headless: no UI, windowing, or Apple frameworks in `include/clay/` or `src/` (Apple dependencies are confined to `backends/metal/` and packaging).

#### Scenario: Three-OS headless build
- **WHEN** CI builds `cpu-only` on macOS, Linux, and Windows runners
- **THEN** the library and full test suite compile and pass on all three

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

