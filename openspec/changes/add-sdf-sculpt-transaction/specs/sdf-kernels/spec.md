# sdf-kernels — relax where the caller owns the volume

Delta for `add-sdf-sculpt-transaction`.

## ADDED Requirements

### Requirement: Relax has an in-place form for a caller that owns its volume
The library SHALL provide relaxing that smooths a caller's volume IN PLACE, alongside the form that copies its input and returns a new one. The two SHALL be the same arithmetic: the in-place form applied to a copy SHALL be byte-identical to the copying form, and a sequence of in-place calls SHALL be byte-identical to the same sequence of copying calls chained through each other's results.

This exists for ownership, not for speed of arithmetic. A live gesture already owns a private working volume, and making it build a second complete result per dab puts a term that scales with the MODEL back into a dab that was made to scale with itself.

#### Scenario: One in-place pass equals one copying pass
- **WHEN** a volume is relaxed in place and a copy of it is relaxed through the returning form with the same settings
- **THEN** the two serialize to the same bytes

#### Scenario: A sequence in place equals the same sequence chained
- **WHEN** two different dabs are applied in place to one volume, and the same two are applied through the returning form one after the other
- **THEN** the two results serialize to the same bytes

#### Scenario: The band narrows the same way
- **WHEN** a volume is relaxed in place
- **THEN** its band is what the returning form would have produced, and it is narrower than the band it started with

### Requirement: Cancelling an in-place operator means whole passes
An operator that rewrites a caller's volume in place CANNOT satisfy the "return the input unchanged" contract that a returning operator satisfies, because the caller's volume IS the working state and there is no input left to hand back.

It SHALL therefore promise the strongest thing available instead: the cancellation checkpoint SHALL sit between whole passes, so a cancelled call has applied some number of complete passes and no fraction of one, and it SHALL report that a later pass was not run. There SHALL be no half-written state for a caller to inspect or discard.

A band SHALL be narrowed by what the COMPLETED passes could have moved the surface, not by what all of them would have. A cancelled call that shrank the band for work it never did would understate the distance a sample-free brick reports, which is the one direction such a bound may not err in.

The returning form SHALL keep its own contract unchanged: a cancelled call SHALL hand back the input.

#### Scenario: A cancel before the first pass applies nothing
- **GIVEN** a token that is already set
- **WHEN** a multi-pass relax is run in place against it
- **THEN** the volume is unchanged sample for sample, its band has not moved, and the call reports itself cancelled

#### Scenario: The returning form still returns its input
- **GIVEN** a token that is already set
- **WHEN** the returning relax is run against it
- **THEN** what comes back is the volume that went in

### Requirement: A region rewrite reports what it selected
A rewrite confined to a region SHALL be able to report what it wrote: the world box spanned by the units of storage it selected, how many it selected, and — separately — whether any stored sample actually moved.

A consumer holding a preview of a volume cannot SEE a rewrite. The volume keeps its identity, its stored set and its bounds, so nothing diffable from outside says which part of it went stale, and the only safe answer without this report is "all of it" — which is exactly the term scaling with the model that a local dab exists to remove.

The box and the count SHALL be GEOMETRIC: they describe what the region selected, not the samples whose values happened to differ. That is what makes them a test's quantity as well as a host's, since the same brush over the same lattice selects the same storage however much unrelated model surrounds it. Whether anything moved SHALL be reported apart from them, because a dab whose weight came out zero everywhere still selected its storage and a consumer still wants to know it has nothing to redraw.

The reporting rewrite and the silent one SHALL be one walk, not two.

#### Scenario: A region-limited pass reports less than the whole volume
- **WHEN** a volume is relaxed over a region covering part of it
- **THEN** the report names more than zero of its bricks and fewer than all of them, and the reported box does not contain a point the region excludes

#### Scenario: A pass that moves nothing still reports where it acted
- **WHEN** a region-limited pass runs at a strength of zero
- **THEN** it reports the bricks the region selected and reports that nothing changed, and every stored sample is what it was

#### Scenario: An empty volume reports nothing
- **WHEN** a rewrite is run over a volume that stores nothing
- **THEN** it reports an empty box, a count of zero, and that nothing changed
