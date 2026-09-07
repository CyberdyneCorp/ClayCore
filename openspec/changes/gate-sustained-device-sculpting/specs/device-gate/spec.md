## ADDED Requirements

### Requirement: A case can be measured over session length, not only document size

The gate SHALL be able to measure a case over HOW LONG THE SESSION HAS BEEN
RUNNING, in windows over one uninterrupted run with no reset between them, in
addition to measuring it over document size.

**The two axes catch different failures and neither substitutes for the other.**
Growth over document size catches an algorithm that is not local. A leak, an
unbounded cache, a history outgrowing its budget and an arena that never
converges do not scale with the document at all, so no case on the size axis can
fail on them however large it is made — and an artist's session is hours of dabs
on one model that is not growing.

A sustained case SHALL record, per window, the latency percentiles, the process
footprint, the thermal state, and the counts of what the engine touched. The
COUNTS carry the verdict, because they are integers the engine produced and are
the same on every machine; the times carry a warning, because a sustained case
is measured on a warm device whose thermal state is expected to have moved.

A record SHALL make visible which axis a case was measured over, rather than
leaving it to be inferred from the case's name.

#### Scenario: A sustained case reports its windows
- **WHEN** a sustained case runs
- **THEN** each window's latency percentiles, footprint, thermal state and counts are recorded, with no reset between windows

#### Scenario: Drift over the session is a verdict of its own
- **WHEN** a sustained case's late window touches materially more or less than its early window, or its per-stamp scratch is still growing in the last window, or its footprint grew while the work did not
- **THEN** the gate reports it as drift over session length, distinctly from growth over document size

#### Scenario: A time-only rise is reported as what it is
- **WHEN** a late window is slower than its early window while the counts held
- **THEN** the report names the thermal states the two were taken in, so a thermal reading is not read as a leak

### Requirement: A sustained fixture must not measure the geometry it is changing

A fixture used to measure session length SHALL hold its own workset constant, so
that a difference between an early and a late window is a property of the
session rather than of the surface the fixture has been deforming.

**A brush whose falloff is measured along the surface changes what the next dab
reaches.** A draw looped on one circle builds a bump, the bump lengthens the
geodesic distance to the same world radius, and a later dab reaches 2.1x fewer
vertices — a fixture that would report a large, confident and meaningless drift.
Alternating that draw's sign does not fix it, because a draw deposits along the
region's averaged normal, which the deformation turns.

The tolerance a sustained case is judged at SHALL be derived from the residue
its fixture actually leaves, and the fixture SHALL be shown to be one that could
fail the gate if it were wrong.

#### Scenario: The fixture returns the surface it started from
- **WHEN** a sustained fixture runs for tens of thousands of dabs
- **THEN** the late window reaches within the stated tolerance of what the early window reached

#### Scenario: A fixture that reads its own geometry is caught
- **WHEN** a fixture whose falloff is changed by its own deposit is used
- **THEN** the drift it produces exceeds the tolerance, and the gate fails rather than reporting a session finding

### Requirement: The fixed-mesh sculpting path is measured on hardware

The fixed-topology mesh brushes SHALL be measured on the reference device rather
than carried as an unmeasured exemption.

Every other case in the harness drives a field or a grid; a mesh brush needs an
imported mesh held as a document layer with its adjacency built once, and until
that fixture existed the classical sculpting mode was covered by nothing on
hardware.

An exemption whose stated blocker has been removed SHALL be deleted rather than
annotated, so the list cannot become a record of things nobody rechecked.

#### Scenario: A mesh brush runs on the device
- **WHEN** the gate runs
- **THEN** a case drives the fixed-topology mesh brush over a document mesh layer and reports a latency figure

#### Scenario: The exemption is gone rather than explained
- **WHEN** the coverage table is read after the fixture exists
- **THEN** the exemption naming the missing fixture is absent, not annotated
