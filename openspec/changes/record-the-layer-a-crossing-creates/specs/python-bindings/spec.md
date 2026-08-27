# python-bindings

## MODIFIED Requirements

### Requirement: Undo covers every reachable edit

With undo enabled, every editing entry point SHALL record its own inverse, so
that no reachable edit escapes undo.

`Document.add_voxel_layer` SHALL apply `AddLayerCmd` like `add_sdf_layer` and
`add_mesh_layer`, rather than inserting into the document directly. It was the
one creation that did not, which made the claim above false wherever a host
converted into a new voxel layer.

#### Scenario: Creating a voxel layer is undoable
- **GIVEN** a document with undo enabled
- **WHEN** a voxel layer is added and the document is undone
- **THEN** the layer is gone
- **AND** redoing brings it back with the same voxel size

#### Scenario: A crossing is one undo step
- **GIVEN** a document with undo enabled
- **WHEN** a voxel layer is created and rasterized into inside one undo group
- **THEN** a single undo removes the layer and its cells together
