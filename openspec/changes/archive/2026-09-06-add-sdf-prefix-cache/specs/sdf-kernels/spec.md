# sdf-kernels — a volume that can be filled in later, and can say what changed

Delta for `add-sdf-prefix-cache`.

## ADDED Requirements

### Requirement: A volume can be created as a lattice with nothing in it
The library SHALL be able to create a sampled volume that carries the LATTICE of a region at a resolution — its index, its unit counts and the bounds a unit reports when it stores nothing — and no stored samples at all.

It SHALL be cheap by construction: no evaluation of any field, and no allocation proportional to what a sampled volume would hold. That is what lets a caller claim its working storage without paying for the model, and fill it in as it turns out to need it.

Sharing a region and a resolution with another volume SHALL mean sharing a lattice, because the lattice origin is the region's own minimum. A caller overlaying one volume onto another depends on that, and it is the difference between reading a stored sample and interpolating two.

Every unit of such a volume SHALL read as storing nothing until something fills it.

#### Scenario: A bare lattice evaluates nothing and stores nothing
- **WHEN** a volume is created as a bare lattice over a region
- **THEN** it holds no stored units, no field was evaluated, and its resolution and band are what were asked for

#### Scenario: Two volumes over one region share a lattice
- **WHEN** a bare lattice and a fully sampled volume are made over the same region at the same resolution
- **THEN** their sample positions coincide exactly

### Requirement: A region can be materialized without re-deciding what is stored
The library SHALL be able to FORCE every unit of storage meeting a region to hold samples produced by a caller's fill, whether or not those values look near the surface, appending to the store rather than rebuilding it. Its cost SHALL be the units it adds, not the units the volume already holds.

This SHALL sit beside, and not replace, the existing region resample. The two differ in who owns the sparsity question:

- The resample RE-DECIDES which units store samples from the values the fill produced, which is what an operator DISPLACING a surface into units that held nothing needs. To do it, it rebuilds the whole store and re-derives the whole lattice's bounds, so it costs what a bake costs.
- Materialization takes sparsity as GIVEN by the caller: a selected unit stores samples afterwards, full stop. It costs what a brush costs.

Forcing rather than classifying is required, not merely convenient. A volume being filled in lazily has to tell "there is no surface here" apart from "nobody has asked yet", and the sentinel for the first already carries a SIGN and a distance that every other reader is entitled to believe. Overloading it with a third meaning would change every consumer of that reading and the stored-form validator besides. So stored-ness SHALL be an honest record of what has been filled in, at the cost of storing units whose samples say nothing interesting.

The bounds a sample-free unit reports SHALL NOT be re-derived by materialization. Re-deriving them is a two-pass sweep over the whole lattice, and a caller materializing a region is by definition about to read INSIDE it, where the stored samples answer.

Materialization SHALL report what it added and what was already there, as counts, so that a caller can state a scaling claim as a number rather than a duration.

#### Scenario: Materializing brings in the region and nothing else
- **WHEN** a region of a bare lattice is materialized
- **THEN** the units meeting that region store samples, every other unit still stores none, and the report counts what was added

#### Scenario: Materializing again brings in nothing
- **WHEN** the same region is materialized a second time
- **THEN** nothing is added, the units are reported as already present, and their samples are untouched

#### Scenario: A unit whose samples are all past the band is still stored
- **WHEN** a region containing no surface is materialized
- **THEN** those units store samples, so that a later reader can tell them from units nobody has asked for

#### Scenario: The cost follows the region, not the volume
- **GIVEN** two lattices over the same region, one of them already holding far more stored units elsewhere
- **WHEN** the same sub-region is materialized in each
- **THEN** the number of units added is the same

### Requirement: A volume can say which of its units went stale, and hand one over
A consumer holding a copy of a sampled volume cannot see what a rewrite did: the volume keeps its identity, its stored set and its bounds. A bounding box says where to look; this says what to FETCH.

The library SHALL name a unit of storage by its lattice coordinate, SHALL report the world position of that unit's first sample, and SHALL be able to read one unit's stored samples out in the order the volume's own sampling and stored form use. Reading a unit that stores nothing SHALL fail and leave the caller's buffer untouched, rather than invent values for it.

A region rewrite SHALL be able to APPEND, to a caller's own list, the coordinate of every unit in which a stored sample actually MOVED — which is not every unit the region selected, and the count of those stays what it is. A consumer transporting a delta wants the units whose bytes are new; a caller sizing work wants the count. Materializing a region SHALL append to the same kind of list, because a unit brought in is new bytes to a consumer exactly as a rewritten one is.

The list SHALL be the CALLER'S, so that a repeated operation reuses one rather than allocating per call, and duplicates across calls SHALL be the caller's to fold.

#### Scenario: A rewrite names the units it moved
- **WHEN** a region-limited rewrite moves samples in some of the units it selected
- **THEN** the appended coordinates are those units, and they are fewer than the units the region selected

#### Scenario: A rewrite that moves nothing names nothing
- **WHEN** a region-limited rewrite is run at a strength that moves no sample
- **THEN** no coordinate is appended, and the count of selected units is still reported

#### Scenario: A unit reads back in the order it was written
- **WHEN** a stored unit is read out by coordinate
- **THEN** its samples are in the same order the volume's sampling produced them, and its reported origin places them in the world

#### Scenario: An unstored unit is refused rather than invented
- **WHEN** a unit that stores no samples is read out
- **THEN** the read fails and the caller's buffer is untouched
