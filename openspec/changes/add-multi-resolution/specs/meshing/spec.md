# meshing — meshing names the level it meshes

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: Voxel meshing names a level
Greedy meshing of a voxel grid SHALL take the level it meshes explicitly, and the form that does not name one SHALL mesh the ACTIVE level. A level a grid does not have SHALL produce an empty mesh rather than a guess.

Meshing is not defined as "always the finest": a block-out pass wants the coarse form at interactive cost, and picking the finest silently would make the level stack unusable for the workflow it exists for.

#### Scenario: The same solid meshes to the same extent at every level
- **WHEN** a grid that has only been subdivided is meshed at two different levels
- **THEN** both meshes span the same world-space extent, because the cell size halved with the cell count

#### Scenario: A one-level grid is unchanged
- **WHEN** a grid with a single level is meshed without naming a level
- **THEN** the mesh is identical to the one the same grid produced before levels existed
