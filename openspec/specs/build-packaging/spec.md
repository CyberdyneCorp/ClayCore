# build-packaging Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: CMake presets and platform matrix
The library SHALL build with CMake presets `cpu-only` (macOS/Linux/Windows), `+metal` (Apple), `+cuda`, and `+opencl`, with warnings-as-errors everywhere and ASan/UBSan jobs in CI. The core SHALL be headless: no UI, windowing, or Apple frameworks in `include/clay/` or `src/` (Apple dependencies are confined to `backends/metal/` and packaging).

The `+cuda` preset SHALL configure whenever a CUDA toolkit is present, however new the installed GPU is. When the detected GPU architecture is one the toolkit can target, the build SHALL target it directly. When it is not — a GPU newer than the toolkit — the build SHALL target the newest architecture the toolkit supports as PTX only, so the driver JIT-compiles for the actual device, and SHALL report both architectures at configure time. An explicit `CMAKE_CUDA_ARCHITECTURES` SHALL override the selection. The selected architecture SHALL be applied to the `claycore` target itself, since the target is created before the CUDA language is enabled and no longer inherits the variable, and SHALL also be applied to the targets created afterwards that device-link against it, so no dependent target links for a different architecture than the code it consumes.

The selection SHALL be made on every configure of a build directory, not only the first. `enable_language(CUDA)` writes its own default into the **cache**, so from the second configure onwards the variable is always set; the build SHALL distinguish that default from a user request rather than treating any set value as explicit. Re-configuring an existing build directory — which `tools/release_check.py --build-dir` does on the directory it is given — SHALL therefore keep the detected architecture rather than silently falling back to the toolkit default.

#### Scenario: Three-OS headless build
- **WHEN** CI builds `cpu-only` on macOS, Linux, and Windows runners
- **THEN** the library and full test suite compile and pass on all three

#### Scenario: GPU newer than the CUDA toolkit
- **WHEN** `cmake --preset cuda` runs on a machine whose GPU architecture the installed nvcc cannot emit a cubin for
- **THEN** configuration succeeds with a PTX-only build of the newest supported architecture, reports the substitution, and the CUDA backend registers and passes the parity suite on that GPU

#### Scenario: Explicit architecture wins
- **WHEN** the build is configured with `-DCMAKE_CUDA_ARCHITECTURES=<arch>`
- **THEN** that value is used unchanged and no fallback is applied

#### Scenario: The detected architecture survives a re-configure
- **WHEN** a build directory configured on a GPU newer than the toolkit is configured a second time without `-DCMAKE_CUDA_ARCHITECTURES`
- **THEN** the same PTX-only architecture is selected and reported again, rather than the toolkit's own default being mistaken for an explicit request

### Requirement: Module dependency rule
Module layering SHALL be enforced (by include checks in CI): `kernel` depends on nothing; `scene`/`brick`/`mesh`/`voxel`/`pick` depend only on `kernel`+`math`; backends depend on `eval`; `io` and bindings sit on top; no module depends on a backend.

The `kernel` module SHALL additionally be publishable on its own: no header
under `include/clay/kernel/` may include a header from any other module, so the
directory can be copied out of the repository and compiled as shader source.

#### Scenario: Layering violation fails CI
- **WHEN** a source file in `scene/` includes a backend header
- **THEN** the include-graph check fails the build naming the offending edge

#### Scenario: Kernel headers stay self-contained
- **WHEN** a header under `include/clay/kernel/` includes `clay/math/` or any other module
- **THEN** the layering check fails, because the published kernels artifact would no longer compile on its own

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

### Requirement: Published kernels artifact
The repository SHALL publish the kernel dialect as a consumable artifact,
`dist/claycore-kernels/`, built by `tools/package_kernels.py` and containing
the headers verbatim under `include/clay/kernel/`, the host parity fixture, and
usage documentation. The headers SHALL be byte-identical to the ones in this
repository: the artifact is a copy, never a transformation, so there is no
second version of the math to keep in step.

`tools/build_xcframework.sh` SHALL place the same headers under each slice's
`Headers/clay/kernel/`, leaving the `claycore` module map covering `clay.h`
alone, so a SwiftPM consumer can point its Metal header search path at the
framework it already links.

#### Scenario: Packaged headers match the repository
- **WHEN** the packaging script runs
- **THEN** every file under `dist/claycore-kernels/include/clay/kernel/` is byte-identical to its counterpart in `include/clay/kernel/`

#### Scenario: The xcframework carries the kernels
- **WHEN** the xcframework is built
- **THEN** each slice's `Headers/clay/kernel/` contains the dialect and the module map still declares only `clay.h`

### Requirement: Host parity fixture
ClayCore SHALL export a machine-readable parity fixture: a set of named cases, each carrying a composed tape (instructions, parameter block, out-of-line blob), a fixed set of probe points, and the CPU scalar reference distance and color at each probe, plus the tolerances a consumer should apply. It SHALL be reachable from the CLI (`clay parity-fixture -o <file>`) and SHALL be deterministic: two exports of the same build are byte-identical.

The case set SHALL cover the surface that a hand-written host preview gets wrong: every blend profile against smooth union, subtraction and intersection; every extended combine mode; the material-mix weights of a colored blend; a deformer chain; and at least one composed multi-layer document.

The case set SHALL TRACK THE KERNEL SET rather than a list fixed when the fixture was written. EVERY combine op the kernel implements SHALL have at least one case exercising it, checked against the op enumeration rather than against a list of names, so an op that ships without a case is a gate failure rather than a discovery. A feature that ships without one leaves the fixture reading as validation while asserting nothing about it, which is worse than absent coverage.

Deformer kinds and primitive families are NOT yet held to that standard: the case set exercises 10 of 20 deformers and 13 primitive opcodes, and the cases that would close those gaps are follow-up work. This is stated rather than implied so a consumer knows what a passing fixture does and does not cover.

Where a feature is a pair sharing one kernel branch with the sign or direction taken from the mode — relief and incise, magnify and pinch — the case set SHALL cover BOTH, since a backend can reproduce one and invert the other.

Each case's probe points SHALL reach the geometry that case exists to exercise. A case whose probes all sit far from its surface records agreement about empty space.

The fixture's own expectations SHALL be gated by the test suite against both the tape interpreter and the registered evaluation backends, so a fixture can never ship expectations that ClayCore itself does not reproduce.

#### Scenario: A host preview that mis-copies a blend fails the fixture
- **WHEN** a consumer evaluates the fixture's tapes with a quadratic smin of support `k` instead of `4k`
- **THEN** at least one probe disagrees with the recorded distance by more than the stated tolerance

#### Scenario: Fixture expectations match the engine
- **WHEN** the test suite evaluates each fixture case through the tape interpreter and every registered backend
- **THEN** the recorded distances and colors agree within the fixture's stated tolerances

#### Scenario: Export is deterministic
- **WHEN** the fixture is exported twice from the same build
- **THEN** the two files are byte-identical

#### Scenario: Every combine op is exercised
- **WHEN** the compiled tapes of the case set are scanned for the combine modes they use
- **THEN** every op in the kernel's combine enumeration appears in at least one case, and paired ops appear in one case per direction

#### Scenario: A case reaches its own geometry
- **WHEN** a case's recorded distances are examined
- **THEN** at least one probe lies near that case's surface rather than all of them in empty space

### Requirement: The SwiftPM library product is statically linked
The `claycore` SwiftPM library product SHALL be declared with an explicit
`type: .static` rather than left automatic.

An automatic product lets the toolchain choose, and Xcode 26 chooses a dynamic
`PackageProduct` framework in Debug builds. The product's only target carries no
code — it exists to hold the `Metal` and `Foundation` linker settings a
`binaryTarget` cannot — so nothing in it references the slice archive and the
linker pulls no objects from it. The framework then exports no `clay_*` symbol
and every consuming application fails to link, while the archive itself contains
every symbol.

A gate SHALL check this declaration in CI. Where a Swift toolchain is available
the gate SHALL read the product's type from SwiftPM itself; where one is not it
MAY read the manifest as text, and SHALL report which of the two it did, so a
passing result is not read as a stronger claim than it is.

The gate SHALL state, in its failure output, why the linkage matters — the
symptom is a wall of undefined symbols in a consuming app and points nowhere
near this manifest.

#### Scenario: An Xcode consumer links
- **WHEN** an application consumes the package on a toolchain that would build an automatic library product as a dynamic framework
- **THEN** the product is static, the archive's objects are linked into the client, and the application's `clay_*` symbols resolve

#### Scenario: The linkage cannot silently revert
- **WHEN** the explicit static declaration is removed from the manifest
- **THEN** the CI gate fails and names the product, the required declaration and the consequence

#### Scenario: The gate does not overstate what it checked
- **WHEN** the gate runs on a machine with no Swift toolchain
- **THEN** it reports that it read the manifest as text rather than through SwiftPM

### Requirement: The kernel package and the tape encoding share a version
The published kernels artifact and the tape encoding a consumer feeds them SHALL carry one version. A host compiles the published headers and feeds them an exported tape; those two only work together, so they SHALL NOT be versioned independently.

The host parity fixture SHALL cover the exported-tape path, not only the fixture's own bundled tapes: a consumer's evaluator agreeing on a fixture and disagreeing on a live document is the failure this fixture exists to prevent.

#### Scenario: The package states the tape version it expects
- **WHEN** the kernels artifact is built
- **THEN** it records the tape encoding version its headers evaluate

#### Scenario: The fixture covers a live tape
- **WHEN** the parity fixture is exercised
- **THEN** it includes a tape obtained through the export path, evaluated by the published headers, and gates it at the same tolerance

### Requirement: The host parity fixture is reachable from the C ABI
The parity fixture SHALL be obtainable through the C ABI, not only by running the command-line tool. A host that consumes this library as a packaged framework runs its tests against that framework and cannot invoke a tool that is not in it, so a fixture reachable only from the CLI is a gate that host cannot run.

The bytes obtained through the ABI SHALL be identical to those the command-line tool writes, and SHALL be deterministic across calls within one build, so that a consumer can diff two runs and attribute any difference to a change it made.

The fixture SHALL carry what a consumer needs to gate its own evaluator without consulting this library again: the composed tapes, the probe points, this library's reference distance and colour at each probe, the tolerances to apply, and the safe step scale a sphere tracer needs.

#### Scenario: A host gates its preview from its own test bundle
- **WHEN** a consumer linking only the packaged library requests the fixture through the ABI and evaluates the same tapes with its own shader
- **THEN** it can assert agreement within the stated tolerances without invoking any tool outside its bundle

#### Scenario: The two producers agree
- **WHEN** the fixture is obtained through the ABI and written by the command-line tool from the same build
- **THEN** the two are byte-identical

#### Scenario: Repeated calls are diffable
- **WHEN** the fixture is requested twice in one process
- **THEN** the bytes are identical, so a difference between two runs is a change in the library rather than in the generator

