# sdf-kernels — move a document's material through the pool

Delta for `batch-the-topological-move`.

## MODIFIED Requirements

### Requirement: A move can be weighted by distance through the material
The library SHALL provide a move whose falloff is weighted by geodesic distance from the anchor THROUGH THE MATERIAL, rather than by Euclidean distance through space, so that parts of a form which are close in space but far along the surface are not dragged together.

The distance SHALL be solved over cells the source reports as material. Free space SHALL NOT be part of the graph, which is what stops the weight crossing a gap.

It SHALL bake, for the reason relax and flatten do: the weight is a solved field rather than a closed form, and putting one in the tape would require a deformer that reads out-of-line data, which no deformer does.

Where a verb samples a source at positions that are NOT the sample lattice, it SHALL be able to take a source that answers a BATCH of arbitrary points, and SHALL ask it once per window rather than once per sample. A topological move is that case: an output sample takes its material from the point the displacement pulls back to, which depends on the geodesic weight there, so no evaluator that knows only the lattice can be told where the queries are. Evaluating a document costs an order of magnitude more per instruction than the arithmetic it performs, so a source asked one point at a time makes the interpreter nearly the whole operation.

The batched form SHALL be indistinguishable from the per-point one, sample for sample. It SHALL route EVERY question it asks the source through the one batched evaluator — both the sampling pass and the material the geodesic walk runs over, the latter being a lattice of cells with no dependency between them and so a single batch.

#### Scenario: A neighbouring part is not dragged
- **WHEN** a topological move is applied to one of two parts that are close in space and joined only through a distant path, with a radius that spans the gap
- **THEN** the neighbouring part is unchanged, where a Euclidean move of the same radius moves it

#### Scenario: The grabbed part still moves
- **WHEN** the same move is applied
- **THEN** the part under the anchor moves in the direction of the drag

#### Scenario: Distance runs along the material
- **WHEN** the radius is raised until it exceeds the path length through the joining body
- **THEN** the neighbouring part begins to move, because it is now within reach along the material

#### Scenario: A move that reaches nothing changes nothing
- **WHEN** the anchor is placed away from any material, or the displacement is zero
- **THEN** the result matches the source

#### Scenario: The declared steepness is measured
- **WHEN** a topological move is applied
- **THEN** the result declares the Lipschitz its samples actually have, rather than an assumed bound

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
