## ADDED Requirements

### Requirement: The brick mesher keeps a crossing off a lattice corner

A vertex placed by the brick mesher SHALL be kept off the endpoints of the edge
it sits on, so that two edges crossing near a shared corner do not place two
vertices at the same point.

A brick stores its samples as `fp16` clamped to a band. Quantisation and band
clamping both drive a sampled value to exactly its neighbour's, and the crossing
parameter to 0 or 1. The resulting triangle has near-zero area and a face normal
that is a cross product of near-parallel edges — numerically garbage, and black
wherever gradient normals are unavailable.

The cost of those triangles is not cosmetic. A host that cannot show them
re-meshes the whole field rather than the bricks an edit touched, and a
whole-field mesh evaluates with no cull, so its cost tracks the document instead
of the edit.

**The guard SHALL apply to the brick path only.** The tape path's vertices SHALL
be unchanged, and callers that deliberately produce degenerate triangles — to
exercise welding and remeshing — SHALL continue to receive them.

The guard SHALL NOT change which triangles exist. The marching case index is
determined by sign tests on the corner values, so the crossing parameter moves a
vertex along its edge and cannot add, remove or reconnect a triangle.

#### Scenario: A brick mesh carries no sliver
- **WHEN** a document is meshed through the brick cache
- **THEN** no triangle's area is negligible against the largest triangle's, and the mesh is watertight and manifold

#### Scenario: The tape path is unchanged
- **WHEN** the same document is meshed from its tape rather than from bricks
- **THEN** the vertices are those the mesher placed before the guard existed

#### Scenario: Topology is unaffected
- **WHEN** a brick mesh is built with the guard and without it
- **THEN** the triangle count is the same, and only vertex positions differ
