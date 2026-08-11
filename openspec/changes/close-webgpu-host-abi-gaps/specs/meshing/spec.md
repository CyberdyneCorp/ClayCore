# meshing — a brick mesh can be built for part of the cache

Delta for `close-webgpu-host-abi-gaps`.

## ADDED Requirements

### Requirement: The brick mesher accepts an explicit brick set
The brick mesher SHALL accept an explicit set of brick keys to march as well as the cache's whole surface set. Marching a subset SHALL sample across the subset's boundary exactly as the whole-surface mesh does, so that a cell's crossing is resolved from the field rather than from what happens to be in the set.

Consequently the triangles a subset mesh produces for a cell SHALL be identical to those the whole-surface mesh produces for that cell. A subset SHALL differ from the corresponding part of the whole only in vertex sharing: a vertex on the boundary of the subset is welded within the subset and emitted again by any other mesh that reaches it, at an identical position. A subset SHALL NOT introduce a crack, a hole, or a displaced boundary vertex.

The mesher SHALL be able to report, for each key in the order given, the contiguous range of vertices and of indices that key contributed. The ranges SHALL partition the output, and the header SHALL state that welding spans brick seams — so a triangle in one key's index range may reference a vertex in an earlier key's vertex range, and a consumer may overwrite a key's ranges but may not free them in isolation.

Whole-surface meshing SHALL remain the default and SHALL remain watertight and 2-manifold, since it is also the export path.

#### Scenario: A subset agrees with the whole
- **WHEN** a cache is meshed whole, and then a subset of its keys is meshed alone
- **THEN** every triangle in the subset mesh appears in the whole mesh at the same world positions, and no cell owned by those keys is missing from either

#### Scenario: A boundary vertex is duplicated, not moved
- **WHEN** two adjacent bricks are meshed as two separate subsets
- **THEN** the vertices on their shared seam appear in both meshes at bit-identical positions, leaving no gap when the two are drawn together

#### Scenario: Export is unaffected
- **WHEN** the whole surface is meshed with no key set named
- **THEN** the result is the watertight, 2-manifold mesh the default mesher already guarantees
