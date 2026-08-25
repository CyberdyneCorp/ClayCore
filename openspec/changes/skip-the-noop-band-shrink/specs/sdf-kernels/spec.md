# sdf-kernels — do not re-derive far bounds a shrink cannot have changed

Delta for `skip-the-noop-band-shrink`.

## MODIFIED Requirements

### Requirement: An operator that transforms a field works on the samples
A field's value where it has no samples is a lower bound, not a measurement. An operator that reads a volume and writes another SHALL work on the stored samples rather than by re-sampling through evaluation, because evaluation mixes measurements with bounds and re-sampling that mixture records the boundary between them as though it were part of the shape.

An operator that MOVES the surface SHALL narrow the band by how far it moved, since the sample-free bricks were classified against where the surface used to be and their bounds would otherwise overstate the distance to where it is now. Narrowing a band that is ALREADY at its floor SHALL cost nothing: what a sample-free brick reports is derived from the stored-brick set, the grid and the band, and an operator that rewrites samples in place changes neither of the first two — so with the band unmoved the derivation reproduces what is already held. This is the ordinary case rather than a corner: a bake begins a couple of cells above the floor, so the first edit of a stroke spends the narrowing and every edit after it asks for one that cannot happen.

Reading a volume's own samples SHALL be proportional to the samples it STORES, not to the lattice its bounds span. A narrow band is sparse by construction — storage is proportional to surface area, and a bake at an interactive cell size stores under a fifth of the points its bounding lattice holds — so a traversal that visits the whole lattice pays for the sparsity twice: once in the points that hold nothing, and again in the sparse lookup each one costs. This applies to the measurement of a volume's sample Lipschitz as much as to the operators that rewrite it.

A traversal over stored bricks SHALL compare a brick's HALO samples, not only the `kBrickDim` per axis a brick owns. A brick stores one extra sample per axis, so a forward pair of adjacent samples always lies wholly inside a single brick — the one holding the lower end below the halo — and dropping the halo drops exactly the pairs that straddle a brick boundary. Such a traversal SHALL agree with one over every point of the bounding lattice, for every volume.

An operator CONFINED TO A REGION SHALL cost what the region contains rather than what the volume contains. A brush whose weight is zero over most of a field still has to be told so once per sample if it walks the whole band, and that reverses the property a brush exists to have.

A traversal so confined SHALL be indistinguishable from the full one, sample for sample, and the operator SHALL be the identity outside the region it declares. That is what makes the two equal, and it is required for a second reason that does not follow from the first: a sample on a brick face is stored by every brick sharing it, and a partial traversal writes only the copies held by the bricks it selected. Were the operator to change such a sample where one sharer was selected and another was not, the copies would disagree and the field would step at the brick face. A brick that was not selected does not meet the region, so every sample it holds lies outside it — which is where the operator is required to do nothing.

A region SHALL be measured to where the operator's weight can be non-zero, INCLUDING any taper, and after any widening the operator applies to that taper rather than as the caller stated it.

#### Scenario: Smoothing does not manufacture a steep edge
- **WHEN** a volume is relaxed and the steepest slope of the result is measured within the sampled region
- **THEN** it has not risen above the slope of the field that went in

#### Scenario: A sample shared by several bricks is found in any of them
- **WHEN** a stored sample lying on a brick face, edge or corner is read by global coordinate
- **THEN** it is found whichever of the bricks sharing it holds the samples

#### Scenario: Narrowing a band already at its floor costs nothing and changes nothing
- **GIVEN** a volume whose band has already been narrowed to its floor
- **WHEN** it is asked to narrow again
- **THEN** the volume is unchanged sample for sample and bound for bound, and the field it reports in the region's empty majority is the same field

#### Scenario: Narrowing a band that CAN narrow still re-derives the bounds
- **WHEN** a volume whose band is above its floor is narrowed
- **THEN** what its sample-free bricks report changes accordingly
