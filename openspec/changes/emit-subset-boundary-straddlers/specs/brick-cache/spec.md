# brick-cache — subset meshing can maintain a surface

Delta for `emit-subset-boundary-straddlers` (issue #66).

## MODIFIED Requirements

### Requirement: A subset of bricks can be meshed
Meshing the cache SHALL accept an explicit set of brick keys as well as the whole surface set, so that a consumer holding a dirty key list can re-mesh what changed rather than what exists. Naming no set SHALL continue to mean every surface brick.

A key in the set that stores no lattice SHALL contribute nothing and SHALL NOT be an error, because a drained dirty set routinely contains bricks that turned out uniform.

The triangles produced for a brick's cells SHALL be those the whole-surface mesh produces for the same cells; a subset SHALL differ only in that a vertex shared with a cell outside the subset is emitted independently, never in that a cell is skipped or a crossing is missed.

A subset SHALL also return the straddlers: every whole-surface triangle with at least one corner inside a requested brick's closed box whose cell is owned by an unrequested brick, each attributed to the lexicographically lowest (x, then y, then z) requested key owning one of its corners. Without them a subset omits triangles no request can name, and no sequence of subset calls can maintain a complete surface — the consumer is forced back onto the whole-surface re-mesh the key list exists to avoid. A subset SHALL NOT contain any triangle the whole-surface mesh does not contain.

Meshing SHALL be able to report, per key in the order given, the contiguous vertex and index ranges that key contributed — its own cells' triangles first, then its attributed straddlers — so that a consumer can write into sub-ranges of a buffer instead of rebuilding it.

#### Scenario: A dab costs the dab
- **WHEN** a small edit dirties a handful of bricks and only those keys are meshed
- **THEN** the work is proportional to those bricks rather than to the surface, and the triangles produced match the corresponding triangles of a whole-surface mesh

#### Scenario: A dab's subset carries its boundary
- **WHEN** an edit's dirty keys are meshed as a subset
- **THEN** every whole-surface triangle with at least one corner inside those bricks is present — the straddlers included — and nothing else is

#### Scenario: A uniform key in the set is ordinary
- **WHEN** a key list contains bricks that are uniformly inside or outside
- **THEN** they contribute no triangles and the call succeeds

#### Scenario: Ranges partition the output
- **WHEN** per-key ranges are requested for a key list
- **THEN** the reported vertex ranges and index ranges are contiguous, non-overlapping, and together cover the whole mesh
