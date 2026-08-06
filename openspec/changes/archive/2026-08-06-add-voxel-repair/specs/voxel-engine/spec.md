# voxel-engine — repair

Delta for `add-voxel-repair`.

## ADDED Requirements

### Requirement: Repair reports before it repairs
The module SHALL provide a non-destructive query returning the number of enclosed empty regions, their total cell count, the largest one's cell count, and whether the grid is airtight. A caller SHALL be able to ask this without performing a repair.

#### Scenario: A hollow shell reports its void
- **WHEN** a hollow box's report is taken
- **THEN** it reports one enclosed void, of the size of the hollow, and not airtight

#### Scenario: A solid block is airtight
- **WHEN** a solid block's report is taken
- **THEN** it reports no enclosed voids and airtight

#### Scenario: A perforated shell is not enclosed
- **WHEN** a hollow box with a hole through its wall is reported
- **THEN** the interior is not counted as an enclosed void, because the outside reaches it

### Requirement: Close holes
The module SHALL provide a repair that seals perforations up to a stated radius by morphological closing over the whole grid. Material SHALL NOT be lost: a closing adds cells and removes none.

#### Scenario: A perforation is sealed
- **WHEN** close-holes runs with a radius wider than a pierced wall's hole
- **THEN** the hole is sealed and the interior becomes an enclosed void

#### Scenario: A large opening is left alone
- **WHEN** close-holes runs with a radius narrower than an opening
- **THEN** the opening remains

#### Scenario: Closing never removes material
- **WHEN** close-holes runs over any grid
- **THEN** every cell occupied before is still occupied

### Requirement: Fill voids
The module SHALL provide a repair that fills every empty cell not reachable from outside the grid's bounds. Reachability SHALL be over empty cells by face adjacency, seeded outside the occupied bounds, so that enclosure is decided rather than guessed at from local neighbourhoods.

#### Scenario: An enclosed void is filled
- **WHEN** fill-voids runs on a hollow box
- **THEN** the interior is solid and the report says airtight

#### Scenario: An open cavity is not filled
- **WHEN** fill-voids runs on a box with an open mouth
- **THEN** the cavity remains empty, because the outside reaches it

#### Scenario: Filling colours from the shell
- **WHEN** an enclosed void inside a single-coloured shell is filled
- **THEN** the filled cells carry that colour rather than an arbitrary palette entry

### Requirement: Repair honours the mask
Both repairs SHALL accept an optional mask and SHALL leave fully masked cells untouched, so that a frozen region is not repaired either.

#### Scenario: A frozen region is not repaired
- **WHEN** fill-voids runs with the void's cells fully masked
- **THEN** the void is left open
