# c-abi — welding

Delta for `add-mesh-weld`.

## ADDED Requirements

### Requirement: Welding is reachable over the C ABI
The C ABI SHALL expose the weld through a versioned descriptor with a defaults accessor, filling a versioned report bounded by the size the caller declared.

Welding a mesh LAYER SHALL respect the same protection every other edit does, and SHALL bump the layer's geometry revision when — and only when — it actually changed something. A weld rewrites the triangles, so it invalidates a cached adjacency, spatial index or sculpting session exactly as a rebuild does; a weld that merged nothing must not invalidate them.

#### Scenario: A weld reports and invalidates
- **WHEN** a mesh layer holding a marched mesh is welded
- **THEN** the report names the triangles collapsed and the tolerance used, and the layer's geometry revision has moved

#### Scenario: A weld that did nothing invalidates nothing
- **WHEN** the same layer is welded a second time
- **THEN** the report says nothing merged, and the geometry revision is unchanged

#### Scenario: A protected layer and a bad tolerance are refused
- **WHEN** a ghosted or locked layer is welded, or a negative tolerance is given
- **THEN** the call is refused and the mesh is unchanged
