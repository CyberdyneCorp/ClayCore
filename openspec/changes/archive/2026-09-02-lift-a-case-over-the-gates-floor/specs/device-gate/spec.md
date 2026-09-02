# device-gate

## ADDED Requirements

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
