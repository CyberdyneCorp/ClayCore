# meshing — a subset mesh emits its boundary straddlers

Delta for `emit-subset-boundary-straddlers` (issue #66). Modifies the
requirement `close-webgpu-host-abi-gaps` added: the subset was complete for
the cells its keys own, but a triangle from a cell owned by an unrequested
brick can still reach a corner into a requested one, and those straddlers
were unreachable by any subset request.

## MODIFIED Requirements

### Requirement: The brick mesher accepts an explicit brick set
The brick mesher SHALL accept an explicit set of brick keys to march as well as the cache's whole surface set. Marching a subset SHALL sample across the subset's boundary exactly as the whole-surface mesh does, so that a cell's crossing is resolved from the field rather than from what happens to be in the set.

Consequently the triangles a subset mesh produces for a cell SHALL be identical to those the whole-surface mesh produces for that cell. A subset SHALL differ from the corresponding part of the whole only in vertex sharing: a vertex on the boundary of the subset is welded within the subset and emitted again by any other mesh that reaches it, at an identical position. A subset SHALL NOT introduce a crack, a hole, or a displaced boundary vertex.

A subset mesh SHALL contain every triangle of the whole-surface mesh with at least one corner inside a requested brick's closed box — including the straddlers, whose cell is owned by an unrequested brick — and SHALL contain no triangle the whole-surface mesh does not. Each straddler SHALL be attributed to exactly one requested key, chosen deterministically as the lexicographically lowest (x, then y, then z) requested key whose closed box contains one of the triangle's corners, so that a consumer holding geometry per brick can dedupe. Straddler vertices SHALL weld onto the subset's own vertices exactly as the whole mesh welds them, at bit-identical positions.

The mesher SHALL be able to report, for each key in the order given, the contiguous range of vertices and of indices that key contributed — the key's own cells' triangles first, then its attributed straddlers. The ranges SHALL partition the output, and the header SHALL state that welding spans brick seams — so a triangle in one key's index range may reference a vertex in an earlier key's vertex range, and a consumer may overwrite a key's ranges but may not free them in isolation.

Whole-surface meshing SHALL remain the default and SHALL remain watertight and 2-manifold, since it is also the export path. It marches every surface cell already, so it has no boundary and owes no straddlers; naming every surface brick as the subset SHALL produce exactly the whole-surface mesh.

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
