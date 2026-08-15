# picking — mesh layers become pickable

Delta for `mesh-fixed-topology-brushes`.

## ADDED Requirements

### Requirement: Raycast against a mesh
The picking module SHALL raycast a `mesh::Mesh` through its BVH and report the hit position, the surface normal, the triangle hit, the barycentric coordinates and the ray parameter.

The query SHALL take the mesh layer's transform and SHALL do the conversion itself — the ray into layer space, the hit back into world space — because a caller doing it by hand gets a brush whose radius changes when a layer is scaled.

The normal SHALL be interpolated from the mesh's own vertex normals when it has them and SHALL fall back to the geometric face normal when it does not, so a mesh imported without normals is still pickable.

A miss SHALL be reported as a miss rather than as a hit at the ray origin.

#### Scenario: A brush gets a surface and a normal
- **WHEN** a ray is cast at a mesh layer whose transform is a translation, a rotation and a scale
- **THEN** the reported position lies on the transformed surface and the reported normal is unit length in world space

#### Scenario: A mesh without normals is still pickable
- **WHEN** a ray hits a mesh carrying no vertex normals
- **THEN** the geometric normal of the hit triangle is reported

#### Scenario: A miss says so
- **WHEN** a ray is cast away from the mesh
- **THEN** the result reports no hit and its position is not used
