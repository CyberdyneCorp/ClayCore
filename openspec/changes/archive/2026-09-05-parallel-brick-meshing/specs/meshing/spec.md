# meshing — bricks march in parallel

Delta for `parallel-brick-meshing`.

## ADDED Requirements

### Requirement: Brick meshing marches in parallel and welds serially
Meshing a set of bricks SHALL march them concurrently and SHALL weld the result through a single builder.

The welding SHALL NOT be sharded per brick. One builder serves every brick so that a lattice edge shared by two of them yields ONE vertex, which is what makes the sparse set watertight at brick seams; per-brick vertex maps concatenated afterwards would duplicate every seam vertex and open the mesh along every brick boundary.

The parallel phase SHALL record what each brick would emit rather than building mesh state, and a serial phase SHALL replay those recordings through the single builder in key order.

The result SHALL be BYTE-IDENTICAL to the serial path — the same vertex array, the same index array and the same per-brick ranges — because the builder receives the same calls in the same order. This SHALL hold as a construction rather than as a tolerance.

Repeated calls for one lattice edge SHALL dedup exactly as they did when the march made them directly.

The per-brick ranges SHALL continue to partition the mesh, which the subset path depends on.

#### Scenario: The mesh does not change
- **WHEN** the same brick set is meshed before and after
- **THEN** the vertices, indices and ranges are byte-identical

#### Scenario: The seams are still welded
- **WHEN** a multi-brick set is meshed
- **THEN** the result is watertight, manifold and oriented, and the ranges sum to the whole mesh

#### Scenario: Repeated runs agree
- **WHEN** the same brick set is meshed many times
- **THEN** every run is byte-identical to every other, so no result depends on which thread reached a brick first

#### Scenario: Meshing from inside a pooled loop
- **WHEN** a caller that is already inside a parallel dispatch meshes bricks
- **THEN** the nested dispatch runs inline and produces the same mesh as a direct call
