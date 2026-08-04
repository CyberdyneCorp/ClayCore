# picking — shape bounds include deformation and repetition

Delta for `add-examples-gallery`. Found while building the repetition example:
`layer_bounds` framed a 5x3x3 grid of spheres as though it were one sphere.

## MODIFIED Requirements

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
