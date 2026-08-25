# sdf-kernels — a relax dab should cost what it moves

Delta for `make-the-relax-dab-local`.

## MODIFIED Requirements

### Requirement: An operator that transforms a field works on the samples
A field's value where it has no samples is a lower bound, not a measurement. An operator that reads a volume and writes another SHALL work on the stored samples rather than by re-sampling through evaluation, because evaluation mixes measurements with bounds and re-sampling that mixture records the boundary between them as though it were part of the shape.

An operator that MOVES the surface SHALL narrow the band by how far it moved, since the sample-free bricks were classified against where the surface used to be and their bounds would otherwise overstate the distance to where it is now.

An operator CONFINED TO A REGION SHALL cost what the region contains rather than what the volume contains. A brush whose weight is zero over most of a field still has to be told so once per sample if it walks the whole band, and that reverses the property a brush exists to have: at an interactive cell size a five-cell brush on a sculpted field spent 96% of its time discovering that samples were outside it.

A traversal so confined SHALL be indistinguishable from the full one, sample for sample, and the operator SHALL be the identity outside the region it declares. That is what makes the two equal, and it is required for a second reason that does not follow from the first: a sample on a brick face is stored by every brick sharing it, and a partial traversal writes only the copies held by the bricks it selected. Were the operator to change such a sample where one sharer was selected and another was not, the copies would disagree and the field would step at the brick face. A brick that was not selected does not meet the region, so every sample it holds lies outside it — which is where the operator is required to do nothing.

A region SHALL be measured to where the operator's weight can be non-zero, INCLUDING any taper, and after any widening the operator applies to that taper rather than as the caller stated it.

#### Scenario: Smoothing does not manufacture a steep edge
- **WHEN** a volume is relaxed and the steepest slope of the result is measured within the sampled region
- **THEN** it has not risen above the slope of the field that went in

#### Scenario: A sample shared by several bricks is found in any of them
- **WHEN** a stored sample lying on a brick face, edge or corner is read by global coordinate
- **THEN** it is found whichever of the bricks sharing it holds the samples

#### Scenario: A region-limited rewrite is the full rewrite
- **GIVEN** a transform that leaves every sample outside a region exactly as it found it
- **WHEN** it is applied over that region only, and again over the whole volume
- **THEN** the two volumes are identical sample for sample, including both copies of every sample shared across a brick face

#### Scenario: A brush leaves the field beyond its taper alone
- **GIVEN** a relax confined to a region, run for several passes
- **WHEN** the samples beyond the region and its taper are compared with the input's
- **THEN** each is equal to the value it had, while samples inside the region have moved

#### Scenario: A dab's cost follows the dab
- **GIVEN** two volumes of the same surface at the same cell size, one covering far more of it
- **WHEN** the same small brush is relaxed into each
- **THEN** what it costs is set by the brush and its taper rather than by how much field surrounds them
