# picking Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
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

