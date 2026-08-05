# build-packaging — the kernels artifact and its parity fixture

Delta for `add-host-kernel-package`.

## ADDED Requirements

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
ClayCore SHALL export a machine-readable parity fixture: a set of named cases,
each carrying a composed tape (instructions, parameter block, out-of-line
blob), a fixed set of probe points, and the CPU scalar reference distance and
color at each probe, plus the tolerances a consumer should apply. It SHALL be
reachable from the CLI (`clay parity-fixture -o <file>`) and SHALL be
deterministic: two exports of the same build are byte-identical.

The case set SHALL cover the surface that a hand-written host preview gets
wrong: every blend profile against smooth union, subtraction and intersection;
every extended combine mode; the material-mix weights of a colored blend; a
deformer chain; and at least one composed multi-layer document.

The fixture's own expectations SHALL be gated by the test suite against both
the tape interpreter and the registered evaluation backends, so a fixture can
never ship expectations that ClayCore itself does not reproduce.

#### Scenario: A host preview that mis-copies a blend fails the fixture
- **WHEN** a consumer evaluates the fixture's tapes with a quadratic smin of support `k` instead of `4k`
- **THEN** at least one probe disagrees with the recorded distance by more than the stated tolerance

#### Scenario: Fixture expectations match the engine
- **WHEN** the test suite evaluates each fixture case through the tape interpreter and every registered backend
- **THEN** the recorded distances and colors agree within the fixture's stated tolerances

#### Scenario: Export is deterministic
- **WHEN** the fixture is exported twice from the same build
- **THEN** the two files are byte-identical

## MODIFIED Requirements

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
