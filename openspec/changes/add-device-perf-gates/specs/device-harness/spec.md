## ADDED Requirements

### Requirement: The harness runs on real device hardware
The device harness SHALL be an XCTest bundle that links the `claycore`
xcframework and drives the **public C ABI** (`clay.h`), run against a
provisioned iPad by `tools/run_device_bench.sh` via `xcodebuild test
-destination 'platform=iOS,id=<udid>'`. It SHALL NOT call internal C++ APIs:
the surface under measurement is the one a host app consumes, and driving it
keeps one harness source valid across branches being compared.

The harness SHALL refuse to report a result it cannot attribute. Before
measuring, it SHALL enumerate the registered backends through the ABI and
record which one served each case, so a build whose Metal backend silently
failed to register cannot be mistaken for a Metal measurement.

#### Scenario: No device attached
- **WHEN** `tools/run_device_bench.sh` runs with no provisioned iPad attached
- **THEN** it exits non-zero naming the missing device, and does not fall back to a simulator or to the host Mac

#### Scenario: The backend that served the case is recorded
- **WHEN** the harness runs a case requesting the `metal` backend on a build where Metal did not register
- **THEN** the case is reported as failed for that backend rather than recorded against the CPU result that served it

#### Scenario: Shader warm-up is excluded from the measurement
- **WHEN** the harness measures the first case on a device with a cold Metal shader cache
- **THEN** a warm-up pass runs before timing begins, so first-run pipeline compilation is not attributed to per-stamp latency

### Requirement: Latency is measured per stamp, at percentiles
Each case SHALL report the wall-clock cost of a **single brush stamp** — the
work between accepting one input sample and having a result the host could
draw — as p50 and p95 over a documented sample count, in milliseconds. Means
SHALL NOT be the reported statistic: the efficiency cores make an occasional
sample an outlier, and a mean lets one stall hide inside a passing average.

Throughput figures (points/sec, bricks/sec) MAY be reported alongside but SHALL
NOT be the gated value. Throughput on a batched corpus is not evidence about
the interactive path.

#### Scenario: A case reports both percentiles
- **WHEN** any device latency case completes
- **THEN** its record carries p50, p95, the sample count, the backend name, the device model and OS version

#### Scenario: p95 is what the budget is checked against
- **WHEN** a case's p50 is inside its declared budget and its p95 is outside it
- **THEN** the gate fails, because a stroke is hundreds of stamps and the tail is what an artist feels

### Requirement: Every case runs across a document-growth axis
Each latency case SHALL be measured on documents of at least three sizes
spanning two orders of magnitude of accumulated stamps, and the record SHALL
carry the measurement per size rather than a single number. Interactive cost
grows with the document, because the tape is recompiled per edit (`c-abi`: a
document compiles its tape once and reuses it until something changes it), so a
single-size measurement pins the least interesting point on that curve.

The gate SHALL check the **growth** as well as the level: a case whose per-stamp
p95 grows super-linearly in stamp count across the axis SHALL fail, naming the
sizes and the measured ratio.

#### Scenario: A regression that only appears on a large document
- **WHEN** a change leaves per-stamp p95 unchanged at the smallest document size but doubles it at the largest
- **THEN** the gate fails on the growth check, even though the smallest size is within tolerance

#### Scenario: Growth is reported even when it passes
- **WHEN** the harness completes a case across every document size
- **THEN** the record carries each size's p50/p95 and the fitted growth ratio, so the curve is inspectable without re-running

### Requirement: Every brush and sculpt verb has a device case
The harness SHALL carry a named coverage table — in the spirit of
`CAPABILITY_EXAMPLES` in `examples/run_all.py` — mapping every brush and sculpt
verb to the device case that exercises it. A verb absent from the table SHALL be
an error rather than a silent gap, and an exemption SHALL be an explicit entry
carrying its reason, so an untested brush is a decision on the record.

The table SHALL cover, at minimum: the voxel sculpt verbs, the SDF verbs
(relax, flatten, move, snakehook, mask paint, mask extrude), the brush stroke
engine, and the cut tool.

#### Scenario: A new verb ships without a device case
- **WHEN** a sculpt verb is added to the engine and the coverage table is not updated
- **THEN** the harness fails naming the uncovered verb

#### Scenario: An exemption is recorded rather than assumed
- **WHEN** a verb is deliberately not measured on device
- **THEN** its table entry carries an explicit exemption with a reason, and the harness passes while reporting it

### Requirement: The device runs the parity corpus, not only the clock
Correctness on device SHALL be gated independently of speed. The harness SHALL
run the backend parity corpus on the iPad, checking every registered backend
against the scalar reference within the tolerances `evaluation-backends`
already documents.

The corpus SHALL include scenes **authored by the brushes** — the tape a stroke,
a cut and a mask extrude actually emit — rather than only hand-written scenes.
A hand-written corpus proves the opcodes agree; it does not prove that what a
brush emits is inside the set that was checked.

The corpus coverage guard SHALL extend beyond primitive types to the combine
ops, the deformers and the blend profiles, so an op that no parity scene reaches
is an error in the same way an unreached primitive already is.

#### Scenario: Metal disagrees with the reference on device
- **WHEN** a parity scene evaluated on the iPad's Metal backend exceeds its documented tolerance
- **THEN** the harness fails naming the scene, the backend, the device, and the worst-case error

#### Scenario: An op no scene reaches
- **WHEN** a combine op, deformer or blend profile is exercised by no scene in the parity corpus
- **THEN** the coverage guard fails naming it

### Requirement: Baselines are committed, and the gate is release-time
Measured results SHALL be written as JSON with a documented schema, and a
baseline SHALL be committed to the repository. `tools/check_device_bench.py`
SHALL compare a run against that baseline and fail on either a regression
beyond a documented tolerance or a case exceeding its declared budget, printing
the case name, the baseline, the measured value and the budget.

Each case SHALL declare an explicit interactive budget in the baseline file. A
case with no declared budget SHALL be an error: an unbudgeted latency number is
a measurement rather than a gate, and this harness exists to gate.

The gate SHALL run in the release workflow and SHALL NOT run in pull-request
CI. Shared runners have no attached iPad and no timing stability, so a blocking
gate there would fail for reasons unrelated to the change under test.

#### Scenario: A release regresses a brush
- **WHEN** a release tag is cut and a device case has regressed beyond tolerance against the committed baseline
- **THEN** the release fails with the case name, baseline, measured value and budget

#### Scenario: A case without a budget
- **WHEN** the baseline carries a case with no declared budget
- **THEN** `tools/check_device_bench.py` exits non-zero naming it, rather than passing the case unchecked

#### Scenario: Baselines record what produced them
- **WHEN** a baseline is committed
- **THEN** it carries the device model, OS version, xcframework version and claycore commit that produced it, so a comparison across different hardware is visibly invalid rather than silently wrong
