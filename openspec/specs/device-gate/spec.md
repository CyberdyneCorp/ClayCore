# device-gate Specification

## Purpose
What a performance number on the reference device is allowed to claim.

A gate that cannot fail protects nothing, so a case counts as measured only
where its figure clears the absolute floor the gate compares against, a batched
case must still measure the path its verb names rather than the batch around it,
and a budget nothing can reach is reported instead of passed. Coverage is
counted over what the gate DECIDES on, not over what it happens to time, and a
figure published anywhere in the tree has to be the one the baseline holds.

Its own capability rather than part of build-packaging because the subject is
the honesty of a measurement, not the making of a build.

## Requirements

### Requirement: A measured case is a case that can fail

A case whose figure cannot clear the gate's absolute floor SHALL NOT be counted
as protected. The gate fails a case only when its growth exceeds both the
tolerance and `NOISE_FLOOR_MS`, so a case gates only above
`NOISE_FLOOR_MS / (tolerance - 1)`; below that it records a number it can never
object to.

Where a verb is genuinely cheaper than that floor, the case SHALL measure a
timed unit large enough to clear it, and the record SHALL say how many
applications of the verb that unit contained. A figure whose scale changed
without the record saying so is a worse report than a figure that could not
fail.

The floor and the tolerance SHALL NOT be lowered to buy sensitivity. Both bound
FALSE failures, and a check that cannot fail is a smaller defect than one that
fails at random.

#### Scenario: A verb cheaper than the floor
- **WHEN** a verb's single application measures below the gate's floor
- **THEN** its case times enough applications to clear the floor
- **AND** the record states how many, so a per-application cost is recoverable

#### Scenario: The report distinguishes protected from merely measured
- **WHEN** the coverage check runs against a run
- **THEN** it reports how many verbs it can fail on and how many it can only report

#### Scenario: An unchanged case keeps its meaning
- **WHEN** a case already clears the floor with one application
- **THEN** it is left alone and its record states a batch of one

### Requirement: A batched case still measures the path its verb names

Enlarging a case's timed unit SHALL NOT change which code path it exercises.

A case whose per-iteration reset is what re-arms the path under test SHALL NOT
be batched, because repeating the body inside one timed unit removes the reset
between repeats and puts every repeat after the first onto a different path.
Such a case SHALL be lifted by extending its own growth axis instead.

A case that already times a compound unit and divides to report a per-item cost
SHALL record the compound figure and its count rather than the quotient. The
quotient is what allows an OPTIMISATION to push a case under the floor and
silently switch off the gate that protects it.

#### Scenario: A case whose reset is the invalidation
- **WHEN** a case's reset is what forces the full path it measures
- **THEN** it is not batched, and its axis is extended instead

#### Scenario: An optimisation cannot switch off its own gate
- **GIVEN** a case that times a compound unit
- **WHEN** the work it measures gets faster
- **THEN** the recorded figure is the compound one, so the case stays able to fail

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
