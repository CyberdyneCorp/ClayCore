## ADDED Requirements

### Requirement: A grid clone copies the grid and not its session
`VoxelGrid::clone()` SHALL return a deep copy of every level, the active level,
the palette and the sculpt layers, and SHALL NOT carry the source's change sink,
pass capture or recording state, since those point into the owner's undo
history. The clone SHALL start with every occupied chunk dirty, a change count
of zero and a cold bounds cache.

#### Scenario: A clone taken mid-gesture records nothing into the source
- **WHEN** a grid with a change sink and a pass capture installed and a sculpt layer recording is cloned, and the clone is edited
- **THEN** the sink and the capture receive nothing, the source's cells and sculpt-layer record are unchanged, and the clone reports no sink and no recording

### Requirement: A level's occupied cells can be enumerated in a stable order
`VoxelGrid::occupied_cells(level)` SHALL return every occupied cell of the
level with its palette index, ordered by z, then y, then x, including the cells
a partially refined level inherits from its parent, and SHALL return nothing for
a level the grid does not have.

#### Scenario: Inherited cells are enumerated where they are
- **WHEN** a level is added over a region and the cells outside it are inherited from the parent
- **THEN** the enumeration's size equals the level's occupied count, eight children per parent cell
