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

### Requirement: A case is judged against the machine it ran on
A budgeted case SHALL record the canary sampled immediately before it began and
immediately after it ended, and the gate SHALL divide that case's measurement by
the resulting slowdown before comparing it to a baseline derived the same way.

Bracketing is required rather than interpolation from periodic samples. A
periodic canary cannot describe a case that ran between two of its readings, and
the device can move from settled to throttled inside one interval: two pooled
cases took it from x1.07 to x1.50 in thirteen seconds, inside a
twenty-five-second gap. A gate that picks whichever sample is nearer returns a
verdict that depends on timer phase, and it reported a 1.6x regression on a verb
measured at 1.05x off-device.

Normalisation SHALL change only the basis of comparison, never hide a slowdown:
a case that got slower on an unchanged machine SHALL still fail, because the same
factor divides both sides.

A case that carries no bracket SHALL be compared raw rather than against an
inferred factor. Guessing is what produced the false failure.

The raw measurement SHALL remain visible in the gate's output, and any check
about what a user would actually wait for SHALL use it: a frame share is about
elapsed time, and a throttled device still spends it.

#### Scenario: The same engine reads the same at both ends of a thermal window
- **WHEN** one unchanged verb is measured on a settled device in one run and on a throttled device in another
- **THEN** the two normalised figures agree within the gate's tolerance, and the raw ones need not

#### Scenario: A real regression still fails
- **WHEN** a verb genuinely slows down and both runs were measured on equally loaded machines
- **THEN** the gate fails it, because the same factor divides both sides

#### Scenario: A case is bracketed rather than interpolated
- **WHEN** a case runs between two periodic canary samples that disagree
- **THEN** its own before-and-after readings decide its factor, and the periodic samples do not

#### Scenario: An unbracketed case is compared raw
- **WHEN** a case carries no bracket, because it predates the field or sets no per-case context
- **THEN** it is compared raw and the gate says so, rather than inferring a factor from nearby samples

#### Scenario: A budget is derived from normalised numbers
- **WHEN** a baseline is written from a run
- **THEN** each budget is derived from the normalised measurement, so a baseline taken on a throttled device does not bake that throttling in as the engine's cost

### Requirement: A drag frame is a measured unit

A gizmo drag SHALL be measured, and its timed unit SHALL be ONE FRAME of the
drag: apply the placement, dirty what it reached, and refill — the same
edit-then-display unit every other interactive case uses, because a host cannot
draw until it has evaluated the result.

A drag case SHALL NOT reset between iterations. A drag has no chain for a reset
to protect: each frame's placement supersedes the last, and the document stays
the size the growth axis names however many frames run. This is the opposite of
the stamp cases, whose reset exists precisely because a stamp grows the
document, and stating it keeps a later reader from adding one for symmetry.

The placement a drag case applies SHALL WALK, and SHALL NOT alternate between
two values. Consecutive frames of a real drag land near each other and dirty
overlapping regions; a two-point flip dirties the same region twice and can be
served from what the previous frame left, so it measures a resume the artist
never gets.

A drag case SHALL record how many bricks one frame refilled, for the same
reason the stamp case does: it separates "the invalidation is too wide" from
"each brick is expensive", which are different defects with different fixes.

#### Scenario: A drag case takes no reset
- **WHEN** a drag case runs its growth axis
- **THEN** the document holds the same number of items at the end of the case as at the start, with no reset having run

#### Scenario: The placement walks
- **WHEN** the placements a drag case applies across one point of its axis are collected
- **THEN** no placement repeats, and consecutive placements are near enough that their influence bounds overlap

#### Scenario: A frame that refilled nothing is a failure
- **WHEN** every timed iteration of a drag case refills zero bricks
- **THEN** the case fails rather than reporting the cost of an empty loop

### Requirement: The transform verbs are covered or exempt

The verbs that place a node or a layer SHALL be part of what the coverage
checker knows about, so a transform entry point without a device case is an
error rather than an absence nobody can see. The checker's verb patterns SHALL
match the placement entry points, and the coverage table SHALL carry an entry —
measured or explicitly exempt — for each.

A placement case SHALL be recorded for a node at the layer ROOT and for a node
inside a GROUP separately. They exercise the same entry point and cost
differently by a factor a gate can hold, because what an edit invalidates is
derived from the node's ancestry rather than from the node; one case standing
for both would report whichever the fixture happened to build.

#### Scenario: A transform verb without a case is named
- **WHEN** a placement entry point exists in the header and the coverage table has no entry for it
- **THEN** the coverage checker fails naming that verb

#### Scenario: Grouped and ungrouped are separate rows
- **WHEN** the run record is read
- **THEN** it carries a node-placement case whose dragged node is at the layer root and another whose dragged node is inside a group, under different case names

### Requirement: The SDF cases are measured with a smooth blend

The SDF stamp and stroke cases SHALL be measured on documents whose items carry
a SMOOTH blend, not only a hard one.

A hard blend contributes nothing to the chain pad — the pad resolves to zero and
holds there — so a hard-blended fixture cannot exercise the cull pad at all, and
every effect that depends on it is invisible to the suite however many cases it
carries. The clay and build brushes are smooth by default, so a hard-blended
fixture is also not the document a sculptor makes.

Where both are worth measuring they SHALL be separate cases rather than one
fixture changed, so neither stands for the other.

#### Scenario: A pad effect is visible to the suite
- **WHEN** a change makes the cull pad move on every append
- **THEN** at least one SDF case reports it, rather than every SDF case being blind to the pad because its fixture is hard-blended

#### Scenario: The smooth and hard shapes are separate rows
- **WHEN** the run record is read
- **THEN** a smooth-blended SDF stroke case and a hard-blended one appear under different case names

### Requirement: A fixture states the regime it measures, and asserts it

A case measuring the brick refill SHALL state the regime its fixture puts the
cache in, and SHALL assert that regime at both ends rather than assume it.

The refill's cost per brick depends on whether the brick straddles the surface
or lies inside the solid, and that is decided by the fixture's geometry and its
resolution together, not by the verb. Every SDF brick case before this one
stamped radius-0.12 spheres into empty space at `voxel_size` 0.05, where one
brick spans 0.4 units: the dab is smaller than a brick, so every brick it
dirties straddles it and none has an interior. The suite therefore measured one
regime while naming the verb, and a change to what an interior brick costs moved
nothing in it.

A case SHALL assert that its filled fixture stores some surface AND that it is
not mostly surface. The first catches a cache that stored nothing — which is
what a refused submit leaves behind, and which reads as a healthy run. The
second catches a fixture that has lost its interior and is measuring a regime
another case already covers.

#### Scenario: A fixture with no interior
- **WHEN** a case's filled fixture is mostly surface bricks
- **THEN** the case fails, because it measures a regime an existing case covers

#### Scenario: A cache that stored nothing
- **WHEN** a case's fill leaves the cache with no surface bricks at all
- **THEN** the case fails rather than reporting the timings it took against it

#### Scenario: Both regimes are measured
- **WHEN** a change alters what an interior brick costs the refill
- **THEN** a detail-resolution case moves and a blockout-resolution case does not
- **AND** neither case alone would have shown it

### Requirement: A submit whose result is discarded is not a measurement

A case driving `clay_brick_cache_submit` SHALL pass `out_results` or
`out_accepted` and SHALL check what came back.

The ABI refuses a submit that asks for neither, because with neither a caller
cannot tell an accepted brick from a stale one. A case that passes neither and
discards the return value evaluates its bricks — which is real, timed, dominant
work — into a cache that stays empty for the length of the run, and nothing in
it measures storage, classification, or a refill reading a populated cache.

#### Scenario: The cache is asked what it accepted
- **WHEN** a case submits evaluated bricks
- **THEN** it asks how many were accepted and fails if that is not all of them

### Requirement: A new suite takes the warm end of the run

A device suite whose baseline does not yet exist SHALL be added at the END of
the run, in its own session, and SHALL NOT be inserted ahead of an existing one.

Run order is measured at 2.7x on a gate whose tolerance is 1.4x, and every
committed baseline was taken in the existing order, so inserting a suite earlier
re-baselines everything below it. Taking the warm end can only make the new
suite's own figures pessimistic, which is the safe direction for numbers nothing
is yet compared against.

A suite SHALL take its own SESSION, and not merely its own bundle, when what it
costs the run is heat rather than memory. A process boundary returns a memory
high-water mark and does not return temperature; only a session boundary with a
cooldown ahead of it starts a suite cold.

#### Scenario: A suite that heats the device
- **WHEN** a case added to an existing bundle takes that session from nominal to serious
- **THEN** it is moved to its own session rather than merely its own bundle

#### Scenario: An existing baseline keeps its position
- **WHEN** a suite is added to the gate
- **THEN** no existing case changes the position it is measured at
