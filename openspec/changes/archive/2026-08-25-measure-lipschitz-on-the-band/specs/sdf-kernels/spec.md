# sdf-kernels — measure the Lipschitz where the samples are

Delta for `measure-lipschitz-on-the-band`.

## MODIFIED Requirements

### Requirement: An operator that transforms a field works on the samples
A field's value where it has no samples is a lower bound, not a measurement. An operator that reads a volume and writes another SHALL work on the stored samples rather than by re-sampling through evaluation, because evaluation mixes measurements with bounds and re-sampling that mixture records the boundary between them as though it were part of the shape.

An operator that MOVES the surface SHALL narrow the band by how far it moved, since the sample-free bricks were classified against where the surface used to be and their bounds would otherwise overstate the distance to where it is now.

Reading a volume's own samples SHALL be proportional to the samples it STORES, not to the lattice its bounds span. A narrow band is sparse by construction — storage is proportional to surface area, and a bake at an interactive cell size stores under a fifth of the points its bounding lattice holds — so a traversal that visits the whole lattice pays for the sparsity twice: once in the points that hold nothing, and again in the sparse lookup each one costs. This applies to the measurement of a volume's sample Lipschitz as much as to the operators that rewrite it.

A traversal over stored bricks SHALL compare a brick's HALO samples, not only the `kBrickDim` per axis a brick owns. A brick stores one extra sample per axis, so a forward pair of adjacent samples always lies wholly inside a single brick — the one holding the lower end below the halo — and dropping the halo drops exactly the pairs that straddle a brick boundary. Such a traversal SHALL agree with one over every point of the bounding lattice, for every volume.

#### Scenario: Smoothing does not manufacture a steep edge
- **WHEN** a volume is relaxed and the steepest slope of the result is measured within the sampled region
- **THEN** it has not risen above the slope of the field that went in

#### Scenario: A sample shared by several bricks is found in any of them
- **WHEN** a stored sample lying on a brick face, edge or corner is read by global coordinate
- **THEN** it is found whichever of the bricks sharing it holds the samples

#### Scenario: The measured Lipschitz does not depend on how the samples were reached
- **GIVEN** a volume whose steepest neighbouring pair lies on a brick's halo sample
- **WHEN** its sample Lipschitz is measured
- **THEN** it reports that pair, and not the shallower one a sweep confined to the samples a brick owns would find

#### Scenario: Measuring a sparse volume does not cost what a dense one would
- **GIVEN** a bake whose stored samples are a small fraction of the points its bounds span
- **WHEN** its sample Lipschitz is measured
- **THEN** the cost tracks the samples stored rather than the lattice spanned
