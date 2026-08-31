# picking Specification

## Purpose
Turning a tap into a place in the model, and saying WHICH part of the model it
landed on.

A raycast against the scene that reports position, normal and attribution — the
layer, and the item where that can be determined — plus surface snapping, the
build plane, bounds and frustum queries, and raycasting a mesh layer directly
because a mesh layer never enters a tape. Protection is honoured here rather
than by the caller: a ghosted layer is not picked.
## Requirements
### Requirement: Scene raycast with attribution
`clay::pick` SHALL raycast against the scene using either the analytic tape or the brick cache (whichever the caller marks as fresher) and return hit position, normal, and attribution (layer id, and item id where determinable).

#### Scenario: Hit attribution
- **WHEN** a ray hits a surface region dominated by a specific edit item
- **THEN** the hit reports that layer and item id, enabling tap-to-select in a consumer app

#### Scenario: Tape vs brick consistency
- **WHEN** the same ray is cast against the analytic tape and against an up-to-date brick cache
- **THEN** hit positions agree within one voxel of the cache resolution

### Requirement: Surface snapping
The module SHALL provide closest-point-on-surface queries by gradient descent on the field, in two modes: position-only and position+normal, converging within a documented iteration budget for points within the narrow band.

#### Scenario: Snap from hover
- **WHEN** a point 2 voxels off the surface is snapped
- **THEN** the result lies on the surface (|f| < epsilon) within the iteration budget, with outward normal when requested

### Requirement: Voxel and build-plane picking
The module SHALL resolve rays against voxel grids (cell + entry face) and against build planes (plane cell under the ray), matching the voxel-engine's grid semantics.

#### Scenario: Face-adjacent placement
- **WHEN** a ray hits an existing voxel's +X face
- **THEN** picking returns that cell and face so a consumer can place the new voxel in the +X neighbor

### Requirement: Bounds and frustum utilities
The module SHALL provide AABB/frustum intersection, bounds of selections (items, groups, layers), and ray–AABB queries for zoom-to-selection and culling. Shape bounds SHALL account for an item's deformer chain and repetition, so a repeated or warped item reports the extent it actually occupies rather than that of one undeformed copy.

#### Scenario: Zoom to selection
- **WHEN** the bounds of a multi-item selection are requested
- **THEN** the returned AABB is the union of the items' influence-bound-tightened shape bounds, transformed to world space

#### Scenario: Repeated item frames every copy
- **WHEN** the layer bounds of a finite grid or radial array are requested
- **THEN** the AABB spans all the copies, not the base primitive alone

#### Scenario: Deformed item frames its swept extent
- **WHEN** the layer bounds of a twisted item are requested
- **THEN** the AABB covers the corners the twist sweeps outward

### Requirement: Ghosted layers are not picked
Ray and surface-snapping queries SHALL ignore ghosted layers, exactly as they ignore hidden ones, while still evaluating them. A locked layer SHALL remain pickable: locking protects against edits, not against selection.

#### Scenario: A ghost in front does not steal the hit
- **WHEN** a ray would hit a ghosted layer before reaching a visible one
- **THEN** the hit reports the visible layer

#### Scenario: Ghosting the only layer means no hit
- **WHEN** every layer a ray would hit is ghosted
- **THEN** the query reports no hit

#### Scenario: A locked layer is still pickable
- **WHEN** a ray hits a locked layer
- **THEN** the hit reports that layer

### Requirement: Raycast against a mesh
The picking module SHALL raycast a `mesh::Mesh` through its BVH and report the hit position, the surface normal, the triangle hit, the barycentric coordinates and the ray parameter.

The query SHALL take the mesh layer's transform and SHALL do the conversion itself — the ray into layer space, the hit back into world space — because a caller doing it by hand gets a brush whose radius changes when a layer is scaled.

The normal SHALL be interpolated from the mesh's own vertex normals when it has them and SHALL fall back to the geometric face normal when it does not, so a mesh imported without normals is still pickable.

A miss SHALL be reported as a miss rather than as a hit at the ray origin.

**The tree a raycast consults SHALL be documented as following the mesh's positions only as far as its last refit or rebuild.** A sculpted mesh whose tree has not been updated SHALL NOT be described as reporting the surface as it was when the tree was built — that is the obvious guess and it is wrong. The hit follows the MOVED triangle, because the position is interpolated from the mesh's current vertices, but it is found through stale bounds, so it drifts OFF the ray. That is invisible to a brush, which wants a depth, and is the whole error budget of a gizmo, which wants a point.

#### Scenario: A brush gets a surface and a normal
- **WHEN** a ray is cast at a mesh layer whose transform is a translation, a rotation and a scale
- **THEN** the reported position lies on the transformed surface and the reported normal is unit length in world space

#### Scenario: A mesh without normals is still pickable
- **WHEN** a ray hits a mesh carrying no vertex normals
- **THEN** the geometric normal of the hit triangle is reported

#### Scenario: A miss says so
- **WHEN** a ray is cast away from the mesh
- **THEN** the result reports no hit and its position is not used

#### Scenario: A stale tree drifts off the ray
- **WHEN** a mesh is sculpted and raycast before and after its tree is updated
- **THEN** the hit before the update lies further from the ray than the hit after it

### Requirement: A mesh BVH can be refitted when vertices move
The mesh BVH SHALL support REFITTING: updating its bounds and its winding-number summaries for a caller-named set of triangles, without rebuilding the tree.

This exists because a mesh layer's topology is fixed. When a brush moves vertices the tree's SHAPE remains a valid partition of the same triangles and only the bounds are stale, so the work is proportional to what moved rather than to the mesh. A rebuild is `O(T log T)` over every triangle; a refit is proportional to the changed triangles and their ancestors.

After a refit the tree SHALL answer every query — raycast, closest point, unsigned distance, winding number — against the mesh's CURRENT positions.

Refit SHALL be CONSERVATIVE: every node's bounds SHALL contain the triangles beneath it. A query that misses geometry because a bound was left too small is a wrong answer, not a slow one, and this is the property that makes refitting safe at all.

A refit naming triangles the caller did not actually move SHALL be correct, merely wasteful. A refit that FAILS to name a triangle that moved leaves that triangle's ancestors too small, so the caller's obligation is to name a superset.

Refit SHALL be refused, changing nothing, against a mesh whose triangle count differs from the one the tree was built over — that is a caller pairing a tree with the wrong mesh, and topology change is exactly what this operation cannot absorb.

A whole-tree refit SHALL be available for a global deformation, where naming the changed triangles would name all of them.

#### Scenario: A refitted tree answers for the moved surface
- **WHEN** a mesh's vertices are displaced and the tree is refitted over the affected triangles
- **THEN** raycast, closest point and winding number agree with a tree freshly built over the displaced mesh

#### Scenario: Bounds stay conservative
- **WHEN** a tree is refitted after a displacement
- **THEN** every node's box contains all three vertices of every triangle beneath it

#### Scenario: Naming too many triangles is harmless
- **WHEN** a refit names every triangle although only a few moved
- **THEN** the result matches a refit naming only the moved ones

#### Scenario: A mismatched mesh is refused
- **WHEN** a refit is asked for against a mesh with a different triangle count
- **THEN** it is refused and the tree is left exactly as it was

### Requirement: A refitted tree reports what its queries cost
The mesh BVH SHALL report a QUALITY measure: the surface-area heuristic's own cost estimate, being the expected number of triangle tests a random ray through the root box must make. Lower SHALL be better, and the measure SHALL be dimensionless and normalised by the root box so it is comparable across meshes and sizes.

A refit keeps a tree CORRECT indefinitely and does not keep it CHEAP: a deformation that stretches triangles swells the boxes holding them, and a query descends both children where it used to descend one. A host SHALL be able to see that happening, by comparing what the tree costs now against what it cost when it was built.

**A rebuild SHALL NOT be presented as the remedy for a degraded score, because measurement does not support it.** Over five deformations of a sphere — a long thin pull, alternating rings pushed apart, a uniform scale, a shear, and a partial collapse to a point — a rebuild produced a BETTER tree than the refit in exactly one, the shear, and by four per cent. In two of them it was dramatically worse: 32.2 against the refit's 20.0 for the pull, and 778.5 against 98.8 for the interleaved case. Median split partitions on triangle CENTROIDS, and a deformation that makes triangles large or elongated defeats that just as thoroughly as it defeats an existing partition — often more, because a refit at least preserves a clustering that was chosen when the geometry was compact.

The library SHALL NOT rebuild on its own behalf, and SHALL NOT advise it on the strength of this number alone. When to spend a rebuild is a host's decision, and the honest guidance is that a rising cost means queries are getting slower, not that a rebuild will fix it.

#### Scenario: Stretching a mesh raises the reported cost
- **WHEN** quality is read on a freshly built tree, and again after a refit that has pulled part of the mesh far away
- **THEN** the second reading is higher

#### Scenario: A uniform scale changes nothing
- **WHEN** every vertex is scaled about the origin and the tree is refitted
- **THEN** the reported cost is unchanged, because the measure is normalised by the root box and the partition is as good as it was

