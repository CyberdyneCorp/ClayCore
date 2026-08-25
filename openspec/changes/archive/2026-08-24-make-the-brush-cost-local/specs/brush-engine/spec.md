# brush-engine

## ADDED Requirements

### Requirement: A mesh dab costs what it moves
Applying a mesh brush stamp SHALL cost in proportion to the geometry it AFFECTS, not to the size of the mesh, whenever the mesh carries a spatial index the operation can consult.

The brush verbs already have this property — they iterate the region and nothing else. The requirement exists because the bookkeeping AROUND them did not: per-class arrays cleared per stamp, a seed found by scanning every class, and a region found by scanning every class. A stamp SHALL NOT clear or scan an array proportional to the mesh in order to record or find something proportional to the brush.

Where a per-class array is needed — the verbs index one by arbitrary ring neighbours — it SHALL be sized once and reset through the list of entries actually written, never cleared wholesale.

#### Scenario: The same dab costs the same on a bigger mesh
- **WHEN** a stamp affecting a fixed number of weld classes is applied to meshes of increasing size, on a mesh whose spatial index exists
- **THEN** the cost is flat rather than growing with the mesh

### Requirement: The brush uses the ray tree it finds and never builds one
Where a mesh brush needs a spatial query — the region under the brush, or the seed a surface walk starts from — it SHALL use the mesh's ray tree when one exists, and SHALL fall back to a scan when one does not. It SHALL NOT build a ray tree on its own behalf.

This is a measured decision rather than a stylistic one: building a tree costs 689 ms on a million-vertex mesh and saves about 1.24 ms per stamp, so a brush that built one would need some five hundred stamps to break even and would make every shorter session worse. A host that places its brush by picking already owns a tree, so the common case is served for free.

The indexed path and the fallback SHALL produce the SAME region, so a brush cannot behave differently according to whether the host happened to have picked. Where the two could differ only by the order in which a set was collected, the order SHALL be made canonical rather than left to the tree's shape — a rebuild changes that shape, and the verbs accumulate float sums over the region.

#### Scenario: A host that never picks is unaffected
- **WHEN** a stamp is applied through a sculptor that has never built a ray tree
- **THEN** the result is identical to the same stamp through a sculptor that has one

#### Scenario: The tree is current before it is consulted
- **WHEN** a region query consults the ray tree after earlier stamps have moved vertices
- **THEN** the tree is refitted first, so the region is the set of vertices that are under the brush NOW
