# picking — Raycast, snapping, interaction math

Delta for `add-claycore-v1`. CPU-side and latency-critical: these run on every Apple Pencil event in the app.

## ADDED Requirements

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
The module SHALL provide AABB/frustum intersection, bounds of selections (items, groups, layers), and ray–AABB queries for zoom-to-selection and culling.

#### Scenario: Zoom to selection
- **WHEN** the bounds of a multi-item selection are requested
- **THEN** the returned AABB is the union of the items' influence-bound-tightened shape bounds, transformed to world space
