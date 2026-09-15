## ADDED Requirements

### Requirement: Exact transient edge welding
The mesher SHALL preserve canonical edge identity, first-encounter vertex numbering and complete mesh output when changing its transient global edge lookup storage.

#### Scenario: Colliding edge hashes
- GIVEN distinct canonical edge pairs with the same lookup hash
- WHEN the mesher interns those edges and then encounters each again
- THEN complete key equality SHALL keep distinct edges separate
- AND repeated edges SHALL return their original vertex indices

#### Scenario: Table growth
- GIVEN enough distinct edges to grow the transient lookup repeatedly
- WHEN the mesh is completed
- THEN positions, indices, normals, colors and brick ranges SHALL match the existing first-encounter reference exactly

#### Scenario: Multiple independent meshes
- GIVEN consecutive or concurrent independent mesh builders
- WHEN each interns edges
- THEN lookup state SHALL remain local to its builder and SHALL be released with that builder
