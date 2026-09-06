## ADDED Requirements

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
