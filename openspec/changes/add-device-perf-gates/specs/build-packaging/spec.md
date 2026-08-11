## MODIFIED Requirements

### Requirement: Versioning
The C ABI and Python API SHALL follow SemVer; kernel headers may evolve freely within a major. The document format version is independent (backward-open, forward-refuse per `file-io`). GPU backend availability SHALL never change results — only speed — enforced by the parity suite as a release gate.

"Only speed" is a claim about the production path, so the release gate SHALL
include the on-device run defined by `device-harness`: parity against the
scalar reference on an attached iPad, and the latency cases against their
committed baselines and declared budgets. A release cut without an attached
device SHALL fail rather than skip the gate — a skipped hardware gate and a
passing one are indistinguishable in a log, and this is the only check that
covers the path the app ships on.

#### Scenario: Release checklist enforced
- **WHEN** a release tag is cut
- **THEN** CI verifies ABI version bump correctness (no symbol/layout break on minor), wheel builds, and parity-suite pass on all registered backends

#### Scenario: Release without an attached device
- **WHEN** a release tag is cut and no provisioned iPad is attached to the release machine
- **THEN** the release fails naming the missing device, rather than reporting the device gate as skipped

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

The iOS device and iOS simulator slices SHALL be built with the Metal backend
enabled, so a host that links the xcframework gets the production path
registered rather than having to wire it per app. The macOS slice SHALL do the
same. Each slice's embedded metallib SHALL be built for that slice's own SDK.

#### Scenario: Packaged headers match the repository
- **WHEN** the packaging script runs
- **THEN** every file under `dist/claycore-kernels/include/clay/kernel/` is byte-identical to its counterpart in `include/clay/kernel/`

#### Scenario: The xcframework carries the kernels
- **WHEN** the xcframework is built
- **THEN** each slice's `Headers/clay/kernel/` contains the dialect and the module map still declares only `clay.h`

#### Scenario: The iOS slice carries a usable Metal backend
- **WHEN** an app links the xcframework's iOS device slice and enumerates backends
- **THEN** `metal` is registered without the app having enabled it, and its metallib loads on the device

### Requirement: Test pyramid
CI SHALL run, in order: kernel unit tests against reference values from `docs/01-sdf-math-foundations.md`; property tests (Lipschitz bounds hold, blends rigid, locality bit-identity); the backend parity suite per registered backend; golden-scene meshing gates (watertight/manifold across the op matrix); I/O round-trip and fuzz tests; and performance benchmarks (points/sec, bricks/sec, mesh time on fixed scenes) with regression gates.

The benchmark tier SHALL be understood as two distinct gates with different
homes. The host benchmarks above are **throughput** gates with deliberately
generous floors, run on shared CI runners, and catch order-of-magnitude
regressions only. Interactive **latency** is gated separately by
`device-harness` on attached hardware at release time, because a shared runner
can neither attach an iPad nor time anything reliably enough to gate on. A
change to the host floors SHALL NOT be read as covering the interactive path.

#### Scenario: Perf regression gate
- **WHEN** a change slows a benchmark scene beyond the configured regression threshold
- **THEN** CI fails with the benchmark name, baseline, and measured value

#### Scenario: The host gate does not stand in for the device gate
- **WHEN** the host benchmark job passes on a pull request
- **THEN** no claim is made about per-stamp interactive latency, which only the device gate establishes

## ADDED Requirements

### Requirement: Interactive measurements are a release-time check
There is no reference tablet in CI, and a job that claims to test hardware it does not have tests nothing — the CUDA and OpenCL jobs already demonstrated that and were removed for it. The interactive measurement SHALL therefore join the checks that are explicitly manual and hardware-dependent rather than be faked as a CI gate.

It SHALL be run before any release that touches the interactive path — evaluation, the brick cache, tape compilation, scheduling or the backends — and its output SHALL be committed with the release.

It SHALL be runnable by one person with one command against a connected device, or it will not be run.

#### Scenario: A release that touches the interactive path
- **WHEN** a release includes a change to evaluation, the brick cache, tape compilation, scheduling or a backend
- **THEN** the interactive measurement has been run on the reference device and its output is committed

#### Scenario: One command
- **WHEN** an engineer with the reference device runs the documented command
- **THEN** the measurement runs on the device and writes its results in the committed format
