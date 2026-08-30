# meshing — welding

Delta for `add-mesh-weld`.

## ADDED Requirements

### Requirement: Coincident vertices can be merged and the collapse removed
The library SHALL provide a verb that merges vertices within a tolerance into one, rewrites the triangles against the survivors, removes any triangle whose corners have become the same vertex, and reports what it did.

This SHALL be distinct from the weld a sculpting adjacency performs. That one groups vertices into classes and leaves every one of them in the mesh, so that a seam's duplicates move together and the triangle list is byte-identical afterwards; this one merges them and rewrites the triangles. Both SHALL resolve "are these the same vertex" through the same code, so a caller welding before building an adjacency does not get two different answers.

The tolerance SHALL be relative to the mesh's own size, matching the existing convention, and a tolerance of zero SHALL merge only bit-identical positions.

#### Scenario: A marched mesh becomes convertible to an adaptive surface
- **WHEN** a mesh produced by the default mesher is welded and then converted to an adaptive half-edge surface
- **THEN** the conversion succeeds, where before welding it was refused for a degenerate triangle

#### Scenario: Welding below the consumer's tolerance is not enough
- **WHEN** a mesh is welded at a tolerance smaller than the one a downstream consumer welds at
- **THEN** that consumer may still refuse it, because it merges pairs the smaller tolerance left apart

### Requirement: Welding preserves what it must
Welding SHALL NOT open a hole: a triangle whose corners coincide bounds no volume, so removing it leaves the surrounding triangles bounding exactly the region they bounded before. A mesh that was watertight before welding SHALL be watertight after.

Welding SHALL NOT merge two vertices whose vertex attributes disagree, unless the caller asks for that explicitly. A UV seam is duplicated positions carrying different UVs — that is how a flat mesh represents a seam at all — and merging across one destroys the layout silently.

Welding SHALL clear the quad list whenever it rewrites the triangles, per the rule that a quad list must never describe triangles that no longer exist. A weld that changed nothing SHALL leave the mesh byte-identical, quads included.

Every index SHALL be within range afterwards: a triangle naming a vertex that does not exist is removed, and that counts as work, so a malformed mesh is not handed back unchanged merely because nothing else needed merging.

The result SHALL be deterministic: the same mesh and tolerance produce the same bytes on every run and platform.

#### Scenario: A watertight mesh stays watertight
- **WHEN** a marched mesh carrying zero-area triangles is welded
- **THEN** the result is watertight, 2-manifold and consistently oriented, and carries no zero-area triangles

#### Scenario: A seam survives
- **WHEN** a mesh whose seam is duplicated positions with different UVs is welded with the default options
- **THEN** no vertices are merged and the seam is intact

#### Scenario: A clean mesh is untouched
- **WHEN** a mesh with nothing to merge is welded
- **THEN** the report says nothing merged and nothing collapsed, and the positions and indices are byte-identical to the input
