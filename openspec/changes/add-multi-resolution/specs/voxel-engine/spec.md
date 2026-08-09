# voxel-engine — sculpt at more than one resolution

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: A grid can carry more than one resolution
A voxel grid SHALL be able to hold a stack of levels, each with half the cell size of the one below it, and SHALL expose adding a level, dropping one, and choosing which is active.

A grid with a single level SHALL behave exactly as a grid does today, so that existing documents and existing calls are unaffected.

#### Scenario: A single-level grid is unchanged
- **WHEN** a grid is built without ever adding a level
- **THEN** every verb, the serialised bytes and the meshed result are identical to the same grid before this change

#### Scenario: Adding a level does not move the surface
- **WHEN** a level is added to a grid
- **THEN** the surface the grid represents is unchanged within the finer level's cell size

### Requirement: Detail survives a change of level
Moving to a coarser level SHALL average, and moving back SHALL restore the finer detail rather than the interpolated approximation, so that working on big forms at a coarse level does not destroy small ones already sculpted.

This is the property the whole feature exists for: without it, stepping down a level is a destructive operation and the workflow it is meant to enable does not work.

#### Scenario: Fine detail survives a coarse edit
- **WHEN** detail is sculpted at the finest level, the active level is dropped, a broad stroke is made, and the finest level is made active again
- **THEN** the broad stroke is present and the fine detail is still there

### Requirement: A verb states its level
Every sculpting and repair verb SHALL act on a stated level, carried by the footprint walk they share so that a verb added later cannot omit it.

#### Scenario: A verb acts only on its level
- **WHEN** a verb runs against a chosen level
- **THEN** the other levels are unchanged except as the detail-preservation rule requires

### Requirement: The dither stays reproducible per level
A falloff brush resolves sub-unit strength by hashing the cell coordinate, and that hash is what makes a stroke reproducible across platforms and backends. Each level SHALL be a uniform lattice with its own cell coordinates, so the hash keeps that guarantee at every level.

#### Scenario: The same stroke at the same level gives the same cells
- **WHEN** the same soft stroke is applied twice to the same level on any platform or backend
- **THEN** exactly the same cells are set
