# sdf-kernels — a flatten dab should cost what it touches

Delta for `make-the-flatten-dab-local`.

## RENAMED Requirements

- FROM: `### Requirement: Flatten samples a new volume rather than editing one`
- TO: `### Requirement: Flatten's result is classified from the flattened values`

## MODIFIED Requirements

### Requirement: Flatten's result is classified from the flattened values
Flatten SHALL decide which bricks its result stores from values the plane has
ALREADY been blended into, so that the new band brackets the flattened surface.
It SHALL NOT reclassify a volume against where the surface used to be, and it
SHALL NOT rewrite an existing volume's samples under a fixed sparse support.

This is why: a narrow band tracks the surface only while the surface stays
inside it. Smoothing moves the surface by less than a cell, so `relax` can
rewrite samples where they lie under support that cannot change. Flatten moves
it by many band widths, and once the surface has walked outside the band there
are no samples left describing where it now is — the isosurface comes apart. So
a brick that held nothing before the blend SHALL be able to hold the facet
after it, and a brick whose samples the blend carried past the band SHALL be
able to stop storing them.

What that forbids is CULLING BEFORE THE BLEND. A brick that looks irrelevant
against the source surface may be exactly where the facet lands, so a source
evaluator's own near-surface decision is not the result's.

Where an exact source exists — a document's field — flatten SHALL prefer it to
a volume, because a volume reports a lower bound rather than a distance outside
its own band, and sampling a field that mixes the two records the boundary
between them as part of the shape. Where flatten must read a volume, it SHALL
prefer that volume's STORED SAMPLE at a position to an evaluation of it, and
fall back to evaluation only where no brick stores one. A stored sample is a
measurement; an evaluation away from the band is a bound that steps by brick,
and re-recording those bounds as though they were distances is what made an
in-place flatten inflate a volume 2.8x and declare a Lipschitz of 14 where its
source declared 1.

#### Scenario: The band brackets the flattened surface
- **WHEN** a shape is flattened so its surface moves well beyond the original band
- **THEN** the result stores samples at the new surface, and none where the old one was

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a flattened volume
- **THEN** it stops at the facet rather than passing through it

#### Scenario: A facet appears where nothing was stored
- **GIVEN** a plane far enough from the surface that the bricks it crosses hold no samples
- **WHEN** the shape is flattened onto it
- **THEN** those bricks store the facet afterwards

#### Scenario: Flattening a volume does not re-record its bounds as distances
- **GIVEN** a volume whose samples are an exact distance, declaring a Lipschitz of one
- **WHEN** a small region of it is flattened
- **THEN** the count of stored bricks and the declared Lipschitz are set by what the brush did, not by the volume having been read back through evaluation

## ADDED Requirements

### Requirement: An operator that moves the surface resamples its region locally
An operator CONFINED TO A REGION that can move the surface OUTSIDE the sampled
band SHALL cost what the region contains rather than what the volume contains,
exactly as a region-limited rewrite does for an operator that cannot. The two
differ only in what they may change: a rewrite preserves which bricks store
samples, a resample decides that again from the values it produced.

A resample SHALL therefore support, for the bricks that meet its region, a
brick that stored samples storing different ones, a brick that stored none
storing some, and a brick that stored some storing none. A brick that does not
meet the region SHALL keep its samples unchanged, byte for byte.

The operator SHALL be the identity outside the region it declares, for the same
two reasons a region-limited rewrite requires it: the bricks that are skipped
keep their old values, which is the same answer only if the operator would have
returned them; and a sample on a brick face is stored by every brick sharing
it, so an operator that changed such a sample where one sharer was selected and
another was not would leave the two copies disagreeing and the field stepping at
the brick face.

The region SHALL be measured to where the operator's weight can be non-zero,
INCLUDING its taper. No margin for how far the surface MOVES is needed and none
SHALL be added on that account: the field outside the region is unchanged, so
its zero set outside the region is unchanged, and whatever surface the operator
creates lies within the region that created it.

The far bounds of sample-free bricks SHALL be re-derived after the sparse
support changes, since they are each a distance to the nearest brick that
stores samples and that set has moved.

The declared Lipschitz SHALL remain one the result's samples satisfy. Bricks
the resample did not touch cannot have become steeper than the volume already
declared, so measuring the bricks it did touch and keeping the volume's own
declaration as a floor is sufficient, and may only overstate.

#### Scenario: A resampled region is the resampled whole
- **GIVEN** an operator that leaves every sample outside a region exactly as it found it
- **WHEN** it is resampled over that region only, and again over the whole volume from the same source
- **THEN** the two volumes store the same bricks and the same samples, including both copies of every sample shared across a brick face

#### Scenario: A dab's cost follows the dab
- **GIVEN** two volumes of the same surface at the same cell size, one covering far more of it
- **WHEN** the same small brush is flattened into each
- **THEN** the number of bricks whose samples are evaluated is the same

#### Scenario: Samples beyond the taper are untouched
- **GIVEN** a flatten confined to a region
- **WHEN** the stored samples beyond the region and its taper are compared with the input's
- **THEN** each is bit-identical to the value it had

#### Scenario: A brush on a brick corner keeps the copies together
- **GIVEN** a brush centred on a brick face, edge or corner, so that selected and unselected bricks share samples
- **WHEN** the result's stored samples are read back by global coordinate from each brick that holds them
- **THEN** every copy of a shared sample agrees

### Requirement: A volume producer measures its Lipschitz once
A verb that builds a volume through the library's sampling SHALL NOT measure
the result's Lipschitz again itself. Sampling already measures what it stored,
over the same samples and by the same rule, and a verb that measures a second
time pays a sweep of every stored sample in the volume — 4.7% of a flatten at
cell 0.015 — to arrive at the number it was already given.

#### Scenario: Sampling's declared bound is the verb's
- **WHEN** a verb that samples a volume returns it
- **THEN** the volume's declared Lipschitz already equals a measurement of its stored samples
