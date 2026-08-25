# sdf-kernels — move a document's material through the pool

Delta for `batch-the-topological-move`.

## MODIFIED Requirements

### Requirement: A topological move drags along the material, not through space
A move SHALL weight its displacement by geodesic distance from the anchor THROUGH THE MATERIAL, so that a part close in space but far along the surface is not dragged with it. Free space SHALL NOT be part of the graph the distance is measured over, however narrow the gap.

Where a verb samples a source at positions that are NOT the sample lattice, it SHALL be able to take a source that answers a BATCH of arbitrary points, and SHALL ask it once per window rather than once per sample. A topological move is that case: an output sample takes its material from the point the displacement pulls back to, which depends on the geodesic weight there, so no evaluator that knows only the lattice can be told where the queries are. Evaluating a document costs an order of magnitude more per instruction than the arithmetic it performs, so a source asked one point at a time makes the interpreter nearly the whole operation.

The batched form SHALL be indistinguishable from the per-point one, sample for sample. It SHALL route EVERY question it asks the source through the one batched evaluator — both the sampling pass and the material the geodesic walk runs over, the latter being a lattice of cells with no dependency between them and so a single batch.

#### Scenario: A drag along a surface does not carry a neighbour across a gap
- **WHEN** two parts lie close in space and far apart along the material, and one is dragged
- **THEN** the other does not move with it

#### Scenario: The batched source and the per-point source agree exactly
- **GIVEN** a document-sourced topological move
- **WHEN** it is run once through a source asked a point at a time and once through one asked a batch at a time
- **THEN** the two volumes are the same sample for sample

#### Scenario: A drag that moves nothing agrees too
- **GIVEN** a move whose displacement is zero, which returns the source sampled unchanged
- **WHEN** it is run through each kind of source
- **THEN** both produce the volume a plain bake of that source produces

#### Scenario: An anchor out of reach of any material agrees too
- **GIVEN** an anchor with no material within the move's radius, so the walk never starts
- **WHEN** it is run through each kind of source
- **THEN** the two agree, and the field is the source's
