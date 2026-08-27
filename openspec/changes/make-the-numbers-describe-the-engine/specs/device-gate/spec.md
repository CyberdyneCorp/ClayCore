# device-gate

## ADDED Requirements

### Requirement: A budget that cannot be reached is reported

A budget sitting far above what its case measures SHALL be reported, with the
case's budget class, so that a budget which has stopped describing its case is
visible rather than inferred.

The comparison SHALL be between the budget and the CURRENT RUN's measurement.
A baseline's budget and its recorded measurement are written from one run and
drift together, so a baseline-internal ratio reads the same for every entry and
detects nothing.

This SHALL report and SHALL NOT fail. A ceiling that is deliberately generous
over content-varying work is legitimate, and the judgement belongs to a reader.

#### Scenario: A budget far above its case
- **WHEN** a case measures many times under its declared budget
- **THEN** the gate reports the ratio and the class, and still passes

#### Scenario: A freshly derived budget is not reported
- **WHEN** a budget was derived from the run it is compared against
- **THEN** it is not reported, because the derived headroom is well inside the threshold

#### Scenario: A case merely fast in one run
- **WHEN** a case measures unusually low in one run against a budget that
  describes it well in others
- **THEN** the threshold is loose enough that it is not named

### Requirement: Coverage judges what the gate decides on

The coverage report SHALL compute a case's sensitivity from the same statistic
the gate compares — the measurement normalised by the machine slowdown that case
ran under — and not from the raw figure.

A case whose raw measurement clears the floor while its normalised measurement
does not SHALL be reported as unprotected, because the gate cannot fail it.

#### Scenario: A case measured on a slow machine
- **GIVEN** a case whose raw p95 clears the floor and whose normalised p95 does not
- **WHEN** the coverage report is produced
- **THEN** the case is reported as one the gate cannot fail

#### Scenario: An unbracketed case
- **GIVEN** a case that carries no canary bracket
- **WHEN** its sensitivity is computed
- **THEN** it is judged raw, because that is how the gate compares it

### Requirement: A published latency figure matches the baseline

A latency figure published in the documentation SHALL agree with the committed
baseline for the device case it names, within a tolerance chosen per bundle, and
a disagreement SHALL fail.

The published figure SHALL be the cost of ONE application of the verb. A case
whose timed body performs several applications SHALL be quoted at its
measurement divided by that count.

A row whose figure deliberately differs from the baseline SHALL carry a recorded
reason, and an exemption naming a row that no longer exists SHALL fail.

#### Scenario: A quoted figure drifts from the baseline
- **WHEN** a published figure and the baseline disagree beyond the tolerance for that bundle
- **THEN** the check fails and names both numbers and the direction

#### Scenario: A batched case is quoted per application
- **WHEN** a case's timed body performs many applications of its verb
- **THEN** the published figure is the per-application cost

#### Scenario: An exemption outlives its row
- **WHEN** a row named by an exemption is removed from the table
- **THEN** the check fails rather than passing on an exemption that applies to nothing
