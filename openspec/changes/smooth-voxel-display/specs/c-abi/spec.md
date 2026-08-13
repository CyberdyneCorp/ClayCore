# c-abi — smooth voxel display

Delta for `smooth-voxel-display` (#108).

## ADDED Requirements

### Requirement: A host can display a voxel sculpt as a form
The C ABI SHALL expose the smooth voxel mesh, so a host can show a sculpt as a rounded form rather than as boxes without meshing the grid itself.

The call SHALL be additive and SHALL sit beside `clay_voxel_mesh` rather than replacing it: the blocky mesh stays reachable, keeps its behaviour byte for byte, and remains what a host uses for export and for hard-surface voxel work.

It SHALL take the smoothing setting as an explicit argument rather than reading a mode from the grid, so two hosts sharing a document cannot disagree about what the grid looks like, and so a host can offer both pictures of one sculpt without mutating it.

An empty grid SHALL yield an EMPTY mesh rather than an error, as `clay_voxel_mesh` does: a grid nobody has drawn in yet is an ordinary state of a session.

#### Scenario: The same grid answers both ways
- **WHEN** a host meshes one grid with `clay_voxel_mesh` and with the smooth call
- **THEN** both succeed, the blocky mesh is byte-identical to what it was before the smooth call existed, and the smooth mesh describes a rounded form over the same occupancy

#### Scenario: Smoothing is the caller's choice, not the document's
- **WHEN** two hosts mesh the same unmodified grid with different smoothing settings
- **THEN** each receives the mesh it asked for and the grid is unchanged by either call
