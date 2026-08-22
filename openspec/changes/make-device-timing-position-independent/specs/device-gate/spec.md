# device-gate Specification

## ADDED Requirements

### Requirement: A case records when it ran
Every case in the run record SHALL carry the offset, in milliseconds from the
start of the run, at which its measurement began, and the thermal state read at
its own start and end. The run-level thermal pair SHALL remain, because it is
what the existing refusal is written against.

#### Scenario: A reader can order the cases by when they ran
- **WHEN** a run record is read
- **THEN** every case SHALL expose its start offset
- **AND** two runs of the same suite SHALL be comparable case by case on that offset

#### Scenario: A case that ran hot says so
- **WHEN** the device's thermal state changes during a case
- **THEN** that case's own start and end states SHALL differ in the record
- **AND** the run-level pair alone SHALL NOT be the only thermal evidence

### Requirement: A canary measures the machine, not the engine
The harness SHALL measure a fixed synthetic workload at least three times per
run — near the start, the middle and the end — and record each sample with its
offset. The canary SHALL exercise no verb that any budgeted case measures, and
SHALL carry no budget of its own.

#### Scenario: The canary is independent of the code under test
- **WHEN** an engine change alters a verb's cost
- **THEN** the canary's samples SHALL be unaffected by that change alone

#### Scenario: The canary spans the run
- **WHEN** a run completes
- **THEN** the record SHALL hold at least three canary samples
- **AND** their offsets SHALL span at least the first and last third of the run

### Requirement: The gate reports drift the thermal state cannot express
`check_device_bench.py` SHALL compute the spread across the canary's samples
and report it on every run. When that spread exceeds a recorded threshold, the
gate SHALL say the run's conditions changed while it ran, and SHALL say so
even when every thermal state read `nominal`.

#### Scenario: A run whose machine changed underneath it
- **WHEN** the canary's slowest sample exceeds its fastest by more than the threshold
- **THEN** the gate SHALL report the drift, its magnitude, and the offsets of both samples
- **AND** it SHALL do so regardless of the thermal states recorded

#### Scenario: A steady run
- **WHEN** the canary's samples agree within the threshold
- **THEN** the gate SHALL report the spread as evidence that position did not matter on this run

#### Scenario: A failure reported against a drifting run
- **WHEN** a case fails a budget or regression check on a run whose canary drifted
- **THEN** the report SHALL name the drift beside the failure, so a position effect is not read as an engine change
