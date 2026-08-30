# python-bindings — welding

Delta for `add-mesh-weld`.

## ADDED Requirements

### Requirement: Welding is reachable from Python
`pyclay` SHALL expose the weld on a mesh, returning what it did as named values, and SHALL raise on a negative tolerance rather than clamping it.

#### Scenario: A marched mesh converts after welding
- **WHEN** a mesh produced by a voxel remesh is welded and then converted to an adaptive surface
- **THEN** the conversion succeeds and the surface's face count matches the welded mesh's triangle count

#### Scenario: The report says what happened
- **WHEN** a mesh with nothing to merge is welded
- **THEN** the returned values report zero merged and zero collapsed
