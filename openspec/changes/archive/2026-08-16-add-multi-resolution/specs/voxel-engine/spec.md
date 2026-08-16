# voxel-engine — sculpt at more than one resolution

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: A grid can carry more than one resolution
A voxel grid SHALL be able to hold a stack of levels, each with half the cell size of the one below it, and SHALL expose adding a level, dropping one, and choosing which is active.

A grid with a single level SHALL behave exactly as a grid does today, so that existing documents and existing calls are unaffected.

Adding a level SHALL subdivide, splitting every occupied cell into its eight children, so the solid the grid represents is exactly the same solid rather than a resample of it. Dropping a level SHALL remove the finest, along with the detail only that level held.

#### Scenario: A single-level grid is unchanged
- **WHEN** a grid is built without ever adding a level
- **THEN** every verb, the serialised bytes and the meshed result are identical to the same grid before this change

#### Scenario: Adding a level does not move the surface
- **WHEN** a level is added to a grid
- **THEN** the surface the grid represents is unchanged within the finer level's cell size

### Requirement: Detail survives a change of level
Moving to a coarser level SHALL average — a coarse cell is occupied when at least half its eight children are, and takes the commonest child's colour — and moving back SHALL restore the finer detail rather than the averaged approximation, so that working on big forms at a coarse level does not destroy small ones already sculpted.

This is the property the whole feature exists for: without it, stepping down a level is a destructive operation and the workflow it is meant to enable does not work.

Each level above the coarsest SHALL therefore hold, alongside its cells, the OFFSETS that distinguish it from the level below: exactly the cells whose value differs from the cell above them. An edit at any level SHALL average down into the coarser levels and replay into the finer ones from their offsets, so the offsets are the authority wherever they exist and derived values fill in everywhere else. An offset the coarser form has caught up with SHALL be dropped, so the record stays the size of the detail rather than the size of the level.

A consequence follows and is intended: a coarse edit SHALL NOT be able to erase detail held at a finer level, because an offset is not something a coarser level can address. Removing that detail is done at the level it lives on.

#### Scenario: Fine detail survives a coarse edit
- **WHEN** detail is sculpted at the finest level, the active level is dropped, a broad stroke is made, and the finest level is made active again
- **THEN** the broad stroke is present and the fine detail is still there

#### Scenario: A trip through another level changes nothing on its own
- **WHEN** the active level is changed and changed back with no edit in between
- **THEN** every level holds exactly the cells it held before

### Requirement: A verb states its level
Every sculpting and repair verb SHALL act on a stated level. The level SHALL be carried by the footprint walk they share rather than added to each verb's arguments, so that a verb added later cannot omit it and no existing signature has to change to gain one.

The stated level is the grid's ACTIVE level, and every cell-addressed query — occupancy, bounds, flood select, picking, the SDF bridges — SHALL read the same one, so that "which level am I on" has a single answer rather than one per entry point.

#### Scenario: A verb acts only on its level
- **WHEN** a verb runs against a chosen level
- **THEN** the other levels are unchanged except as the detail-preservation rule requires

#### Scenario: A mask selects the same region at every level
- **WHEN** the same world-addressed mask gates the same verb at two different levels
- **THEN** the same world-space region is protected at both, because the walk converts a cell to world space through the active level's cell size

### Requirement: The dither stays reproducible per level
A falloff brush resolves sub-unit strength by hashing the cell coordinate, and that hash is what makes a stroke reproducible across platforms and backends. Each level SHALL be a uniform lattice with its own cell coordinates, so the hash keeps that guarantee at every level.

#### Scenario: The same stroke at the same level gives the same cells
- **WHEN** the same soft stroke is applied twice to the same level on any platform or backend
- **THEN** exactly the same cells are set
