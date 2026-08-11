# performance-budgets — what the interactive path is expected to cost

Delta for `add-device-perf-gates`. This capability is new.

Adopted from `add-device-perf-budgets`, which proposed it independently and was
folded into this change rather than run twice. Its reasoning is kept because it
is better than restating: the budget is a property of the PRODUCT, not of the
build, so `build-packaging` is the right place for *how* the measurement runs
and the wrong place to say what a brush dab is expected to cost on a named
device.

Four of its requirements are already met by the harness this change builds; two
— sustained behaviour, and the preview frame — are not, and are tasks rather
than claims.

## ADDED Requirements

### Requirement: The interactive budget is stated, not assumed
The engine SHALL state the latency budget it targets for interactive use, on a NAMED reference device, at a NAMED document size. A budget without a device and a size is not a requirement and SHALL NOT be recorded as one.

The budget SHALL be the one the consuming application imposes: a brush dab completing within the interval between input events (4–8 ms at 120–240 Hz), and a preview frame within a 60 fps frame (16.7 ms).

Where the engine does not meet the budget at some document size, the specification SHALL record the size at which it does and the size it targets, rather than being weakened until it passes.

#### Scenario: The budget names its device
- **WHEN** the performance requirement is read
- **THEN** the reference device, the OS version and the document size the budget applies to are all stated

### Requirement: The interactive path is measured end to end
Measurement SHALL cover the whole path a gesture travels — marking dirty, draining requests, evaluating, submitting, and for a preview also meshing or raycasting — and SHALL report the total.

Per-stage microbenchmarks SHALL be kept, because a total that moves has to be attributable to a stage, but a stage measurement SHALL NOT stand in for the total. A stage that got faster while the total got slower is a result the measurement is required to be able to produce.

#### Scenario: A dab is timed as a dab
- **WHEN** the interactive measurement runs
- **THEN** it reports the time for a complete dab at each stated document size, not the time for its stages alone

#### Scenario: A regression is attributable
- **WHEN** the total moves between two runs
- **THEN** the per-stage figures from the same run are available to attribute it

### Requirement: The numbers come from the target device
Interactive measurements SHALL be taken on the reference device, on arm64, on real hardware. A simulator SHALL NOT be accepted as a source for them: it runs the host's cores and cannot answer a question about a thermally limited SoC.

Results SHALL be recorded in the repository, naming the device, the OS version, the build configuration and the commit, so that a change in the numbers is a diff rather than a recollection.

#### Scenario: A simulator result is not a device result
- **WHEN** measurements are taken in a simulator
- **THEN** they are labelled as such and do not satisfy this requirement

#### Scenario: The numbers are in the repository
- **WHEN** a measurement run completes
- **THEN** its results, device, OS, build and commit are committed alongside the code they describe

### Requirement: Sustained behaviour is measured, not assumed
Measurement SHALL include a sustained run and SHALL report the first-dab figure and the steady-state figure separately, because the number that decides whether the app is usable is the one after the device has warmed up, not the one from a cold start.

#### Scenario: Steady state is reported
- **WHEN** the sustained measurement runs
- **THEN** it reports both the initial and the settled figures, and the interval over which the difference appeared

### Requirement: Backend choices are justified by measurements on the target
A recorded decision about WHICH backend runs a workload — such as keeping brick fills on the CPU because the CPU/GPU crossover sits above a brick's sample count — SHALL be supported by a measurement on the reference device, or SHALL be marked as provisional and name the machine it was actually taken on.

#### Scenario: A provisional decision says so
- **WHEN** a backend routing decision rests on a measurement from a machine other than the reference device
- **THEN** the decision is recorded as provisional, naming that machine
