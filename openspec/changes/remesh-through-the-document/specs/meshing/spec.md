# meshing — what a rebuild reports, and how it projects

Delta for `remesh-through-the-document`.

## ADDED Requirements

### Requirement: A voxel remesh reports what it cost and how far it strayed
The report of a voxel remesh SHALL carry the result's distance to the source surface as a root-mean-square, a 95th percentile and a maximum; the wall clock spent in each stage; and the working-memory figure the resource guard compared against the budget.

The distance SHALL be documented as ONE-SIDED — every result vertex against the source — and the API SHALL say what that cannot see: a source feature the result deleted entirely leaves no result vertex near it to report, so a rebuild that lost a spike scores well on it. The estimate's thin-feature warning and the relative volume difference are what answer that question.

The stage timings SHALL be diagnostics rather than a contract: they vary between runs, and nothing about the resulting mesh does.

The memory figure SHALL NOT be described as a measured peak. The library has no allocator hook; the number is what the guard computed.

#### Scenario: A finer rebuild strays less
- **WHEN** a model is rebuilt at two resolutions, one finer
- **THEN** the finer rebuild's root-mean-square distance to the source is smaller

#### Scenario: The timings account for the run
- **WHEN** a rebuild succeeds
- **THEN** every stage has a non-negative time and their sum is within the call's own duration

### Requirement: Source projection weights an incompatible sheet rather than rejecting it
Where a voxel remesh projects its result onto the source, the movement of each vertex SHALL be scaled continuously by how well the source surface there faces the same way as the vertex's own reconstructed normal, reaching zero where it faces away.

A hard rejection SHALL NOT be used. Moving a vertex fully while leaving its neighbour untouched tears the surface into itself: measured on a sheet folded back through itself, a hard rejection left self-intersecting triangle pairs where the unprojected surface had none and where the continuous weight leaves none.

Projection SHALL NOT increase the result's self-intersection count, and SHALL NOT increase its distance to the source.

#### Scenario: Projection does not tear a fold
- **WHEN** a surface that folds back through itself is rebuilt with projection on and off
- **THEN** the projected result has no more self-intersecting triangle pairs than the unprojected one, and is no further from the source

#### Scenario: The weight is exercised, not merely present
- **WHEN** that same fold is rebuilt
- **THEN** a measurable share of the vertices within the projection clamp have a closest source point whose surface faces away from them
