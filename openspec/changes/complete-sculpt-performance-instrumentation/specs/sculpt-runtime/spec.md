## ADDED Requirements

### Requirement: A stamp reports what it did as well as how long it took

A sculpting stamp SHALL be able to report, per stage, both the time it spent and
the WORK IT DID — the vertices it considered and the vertices it affected, the
faces and chunks it touched, the neighbours it gathered, the topology operations
it performed, the detail blocks it wrote and the scratch it peaked at.

**A duration alone cannot say why a duration changed.** A stage that got slower
because it touched twice as many vertices and a stage that got slower because
its inner loop regressed are the same number, and a regression report that
cannot tell them apart sends someone to the wrong file.

The counts SHALL come from ONE record. Several of them already exist, each on
its own accessor with its own shape; a regression that has to assemble a stamp's
story from six places will not be assembled.

Collecting them SHALL cost nothing when nothing is collecting: one predictable
branch, no allocation on a warm dab, no formatting and no lock in a hot loop —
the discipline the stage timings already follow.

#### Scenario: A stamp reports its counts
- **WHEN** a stamp is made with a counter record attached
- **THEN** the vertices it considered and the vertices it affected are reported, and they differ where the falloff's rim or a mask held some of them still

#### Scenario: Nothing attached costs nothing
- **WHEN** a stamp is made with no record attached
- **THEN** no counter is accumulated and no clock is read

### Requirement: All three representations report in one vocabulary

The fixed mesh, the adaptive surface and the multiresolution hierarchy SHALL
report their stages and counts in the same vocabulary, so that a row from one
can be compared with a row from another.

The adaptive surface is the representation whose per-dab cost is hardest to
predict, because it splits, collapses and flips as it goes, and it is the one
that carries no stage breakdown today.

Where a representation has no work for a stage, it SHALL report zero for that
stage rather than omitting it, so that a missing stage and an unused one are
distinguishable.

#### Scenario: An adaptive stamp reports its topology work
- **WHEN** a stamp on an adaptive surface splits or collapses elements
- **THEN** the topology stage reports both the time it took and how many splits, collapses and flips it performed

#### Scenario: The three can be compared
- **WHEN** the same brush is applied to each of the three representations with a record attached
- **THEN** each reports the same stage names, and a stage a representation does not use reports zero

### Requirement: The breakdown is gated by tests, not by one benchmark

Every stage SHALL be covered by a test asserting that it is reached and reports
work, so that a stage whose timing is removed in a refactor fails a gate rather
than quietly reporting zero.

**Zero nanoseconds and zero calls is what an unmeasured stage looks like, and it
is also what an unused one looks like.** With thirteen stages, one consumer and
no tests, a stage can stop being measured without anything saying so.

The gates SHALL assert COUNTS rather than durations, which are deterministic
where a duration is a claim about the machine that ran it.

#### Scenario: A stage that stopped being timed
- **WHEN** a stage's timing is removed
- **THEN** a test fails, rather than the stage reporting zero to the one benchmark that reads it
