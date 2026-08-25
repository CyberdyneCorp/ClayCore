# meshing — mesh the cells no surface brick owns

Delta for `mesh-the-cells-no-brick-owns`.

## MODIFIED Requirements

### Requirement: The brick mesher accepts an explicit brick set
The brick mesher SHALL accept an explicit set of brick keys to march as well as the cache's whole surface set. Marching a subset SHALL sample across the subset's boundary exactly as the whole-surface mesh does, so that a cell's crossing is resolved from the field rather than from what happens to be in the set.

Consequently the triangles a subset mesh produces for a cell SHALL be identical to those the whole-surface mesh produces for that cell. A subset SHALL differ from the corresponding part of the whole only in vertex sharing: a vertex on the boundary of the subset is welded within the subset and emitted again by any other mesh that reaches it, at an identical position. A subset SHALL NOT introduce a crack, a hole, or a displaced boundary vertex.

EVERY CELL THAT CROSSES SHALL BE MARCHED, and marching the cells that surface bricks own is not that. A cell is owned by the brick its low corner falls in and takes its other seven corners from up to seven neighbours, so a cell owned by a brick that stores no lattice — uniformly inside, uniformly outside, or never evaluated — crosses whenever one of those neighbours holds a sample of the opposite sign. This requires the field to move more than a band across one voxel step, which a 1-Lipschitz distance field never does and a worked document does routinely: a displacement applied over a region narrower than the displacement is steeper than the band, and the tape declares that through its safe step scale. The mesher SHALL mesh those cells rather than assume the classification covers them. Whether a brick's own samples cross is a property of one brick; whether a cell crosses is a property of eight, and only the mesher sees all eight.

A subset mesh SHALL contain every triangle of the whole-surface mesh with at least one corner inside a requested brick's closed box — including the straddlers, whose cell is owned by an unrequested brick — and SHALL contain no triangle the whole-surface mesh does not. Each straddler SHALL be attributed to exactly one requested key, chosen deterministically as the lexicographically lowest (x, then y, then z) requested key whose closed box contains one of the triangle's corners, so that a consumer holding geometry per brick can dedupe. Straddler vertices SHALL weld onto the subset's own vertices exactly as the whole mesh welds them, at bit-identical positions.

A cell whose owner stores no lattice SHALL be attributed WHOLE instead, to the lexicographically lowest requested key whose closed box contains one of the CELL's eight corner lattice points. The per-corner rule cannot be used for it: no other request marches that cell, and its crossing vertices lie inside the owner's box and strictly outside every requested brick's, so filtering by corner would drop every triangle it produces and reopen the hole. The cell touched the request; that is what decides.

The mesher SHALL be able to report, for each key in the order given, the contiguous range of vertices and of indices that key contributed — the key's own cells' triangles first, then its attributed straddlers. The ranges SHALL partition the output, and the header SHALL state that welding spans brick seams — so a triangle in one key's index range may reference a vertex in an earlier key's vertex range, and a consumer may overwrite a key's ranges but may not free them in isolation.

Whole-surface meshing SHALL remain the default and SHALL remain watertight and 2-manifold, since it is also the export path — and it SHALL collect straddlers to stay so, rather than relying on owning every cell that crosses. Naming every surface brick as the subset SHALL produce exactly the whole-surface mesh.

#### Scenario: A subset agrees with the whole
- **WHEN** a cache is meshed whole, and then a subset of its keys is meshed alone
- **THEN** every triangle in the subset mesh appears in the whole mesh at the same world positions, and no cell owned by those keys is missing from either

#### Scenario: A straddler is emitted, not omitted
- **WHEN** an edit's dirty keys are meshed as a subset and the whole surface is meshed from the same cache state
- **THEN** the triangles with at least one corner inside the requested bricks are the same set in both meshes — none missing from the subset, none invented by it

#### Scenario: A per-brick consumer can maintain a surface
- **WHEN** a consumer replaces each dirty key's stored share from the subset's per-key ranges after every edit, deduplicating repeated triangles
- **THEN** the union of the stored shares equals a whole-surface rebuild of the same document

#### Scenario: A boundary vertex is duplicated, not moved
- **WHEN** two adjacent bricks are meshed as two separate subsets
- **THEN** the vertices on their shared seam appear in both meshes at bit-identical positions, leaving no gap when the two are drawn together

#### Scenario: Export is unaffected
- **WHEN** the whole surface is meshed with no key set named
- **THEN** the result is the watertight, 2-manifold mesh the default mesher already guarantees

#### Scenario: A field steeper than the band does not punch holes
- **GIVEN** a document worked hard enough that its declared safe step scale is far below one — a ball with a ring of relief dabs, every third one incised
- **WHEN** its brick cache is meshed whole, and again by naming every surface brick
- **THEN** both meshes are watertight, 2-manifold and consistently oriented, with no open boundary edge, exactly as a whole-document mesh of the same document is

#### Scenario: A well-bracketed field is unchanged
- **WHEN** a document whose field the band does bracket is meshed from its brick cache
- **THEN** the mesh is byte-identical to the one the owner rule alone produced, because no cell owned by a lattice-less brick crosses
