# voxel-engine — sculpt layers

Delta for `add-sculpt-layers`.

## ADDED Requirements

### Requirement: A sculpting pass is recordable and adjustable
A voxel grid SHALL be able to record a sculpting pass as a layer holding the cells the pass changed and their previous values, with a strength in [0, 1] and a visibility flag, composited in order.

This is distinct from undo. Undo is a stack, so removing an old pass discards everything after it; a sculpt layer is addressable and can be revisited at any time without losing later work.

#### Scenario: A pass at zero strength is as if it never ran
- **WHEN** a pass is recorded and its strength set to 0
- **THEN** the grid is identical to before the pass

#### Scenario: A pass at full strength equals applying it directly
- **WHEN** a pass is recorded and its strength set to 1
- **THEN** the grid is identical to applying the same verbs without recording

#### Scenario: An old pass is adjusted without losing later work
- **WHEN** several passes are made and the strength of the first is lowered
- **THEN** the later passes are still present and the first contributes proportionally

### Requirement: Fractional strength on binary occupancy is reproducible
Occupancy is binary, so a fractional strength cannot mean a fraction of a cell. A partially applied pass SHALL select its cells by dithering against the same cell-coordinate hash the falloff brushes use, so the result is reproducible across platforms and backends as a brush stroke already is.

#### Scenario: The same fractional strength gives the same cells
- **WHEN** a pass is set to the same fractional strength twice, on any platform or backend
- **THEN** exactly the same cells are occupied
