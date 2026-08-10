# build-packaging — the device measurement is a release obligation

Delta for `add-device-perf-budgets`.

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
