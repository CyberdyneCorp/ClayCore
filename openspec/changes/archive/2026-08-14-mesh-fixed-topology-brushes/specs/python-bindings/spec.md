# python-bindings — sculpting a mesh layer from Python

Delta for `mesh-fixed-topology-brushes`.

## ADDED Requirements

### Requirement: MeshSculptor
`pyclay` SHALL expose a `MeshSculptor` constructed over a `Mesh` — including the borrowed mesh a mesh LAYER hands back, so sculpting a layer edits the document's own triangles rather than a copy.

It SHALL expose one stamp, one whole stroke, a ray query, a BVH refresh, and the counts a caller needs to see the structure it built.

Sculpting a borrowed mesh whose layer has been removed SHALL raise, as every other borrowed-mesh access already does.

A LOCKED or GHOSTED layer SHALL refuse to be sculpted, because those flags mean "never edited" and a vertex displacement is an edit.

#### Scenario: A mesh layer is sculpted in place
- **WHEN** a document's mesh layer is fetched, sculpted through a `MeshSculptor`, and the layer is read back
- **THEN** the layer's positions show the edit and its indices and quads are unchanged

#### Scenario: A protected layer refuses
- **WHEN** a sculptor is asked to stamp a mesh borrowed from a locked or ghosted layer
- **THEN** it raises and the mesh is unchanged

### Requirement: The verbs and their settings from Python
`pyclay` SHALL expose the verb set as an enumeration and the brush settings as keyword arguments with the same defaults the C++ header declares, so a Python caller and a C caller reading the same documentation get the same stroke.

The signed convention SHALL be preserved: one `pinch` verb whose negative strength magnifies, and one `flatten` verb taking the `TwoSided` / `CutOnly` / `FillOnly` mode.

Invalid arguments SHALL raise rather than be clamped, matching the C ABI's refusals so the two do not disagree about what a value means.

#### Scenario: The bindings agree with the ABI about a refusal
- **WHEN** a non-positive radius or an out-of-range iteration count is passed
- **THEN** Python raises and C returns `CLAY_ERROR_INVALID_ARGUMENT`

### Requirement: VertexDeltas from Python
`pyclay` SHALL expose the vertex-delta record with its vertex count, revert, apply and clear, and it SHALL be usable as one undo step across a whole gesture.

#### Scenario: Undo from Python is bit-exact
- **WHEN** a stroke is applied with a record and the record is reverted
- **THEN** the mesh's positions and normals compare equal bit for bit to a copy taken before the stroke

### Requirement: Binding parity holds
`tools/check_binding_parity.py` SHALL pass with every capability added here reachable from the C ABI, or exempt with a stated reason.

#### Scenario: The gate passes
- **WHEN** the parity gate runs against the built module and `clay.h`
- **THEN** it reports no unmatched capability and no stale exemption
