# voxel-engine — an edit's effect is observable

Delta for `report-voxel-edit-effect`.

## ADDED Requirements

### Requirement: Whether an edit changed anything is observable
A voxel grid SHALL expose a monotone count of the cell writes that actually changed a cell, since the grid was constructed. A changed cell SHALL mean a write that changed the stored palette index — empty to occupied, occupied to empty, or one index to another — so rewriting a cell with the index it already holds SHALL NOT be counted, and a palette recolour, which touches no voxel data, SHALL NOT move it.

The counter SHALL be maintained in the single mutation funnel every editing operation writes through, so that every brush, fill, sculpting verb, repair and rasterization is instrumented by construction rather than verb by verb.

The difference between two reads SHALL be zero exactly when the grid is byte-identical to what it was before the bracketed calls. That difference SHALL further be the exact number of changed cells for every verb that writes each cell at most once; for the two verbs that may write a cell twice within one call — pinch and magnify, which clear a cell and write its colour into a neighbour the same call may later visit — it SHALL be an upper bound, and the spec SHALL state so rather than claiming an exactness it does not have.

The count SHALL be monotone and never reset, so only differences are meaningful. It SHALL NOT be reachable through the occupied-cell count, which cannot answer the question: a verb that moves material without adding any leaves that count alone.

#### Scenario: A sub-cell grab is a valid edit that moves nothing
- **WHEN** a grab is applied with a displacement under half a cell on every axis
- **THEN** the call succeeds, the serialized grid is byte-identical, the occupied count is unchanged, and the change count is unmoved

#### Scenario: A sub-cell smudge is the same case
- **WHEN** a smudge is applied with a displacement under half a cell on every axis
- **THEN** the call succeeds, the serialized grid is byte-identical, and the change count is unmoved

#### Scenario: A supra-cell drag is distinguishable from a dead one
- **WHEN** the same grab or smudge is applied with a displacement over a cell
- **THEN** the serialized grid differs and the change count has moved

#### Scenario: The occupied count cannot tell the two apart
- **WHEN** a grab whose footprint is far wider than the material translates it rigidly by a cell, and the same grab is applied with a sub-cell displacement
- **THEN** the occupied count reads identically for both, while the change count separates them

#### Scenario: Rewriting a cell with what it already holds is not a change
- **WHEN** a cell is set to the index it already holds, or an already-empty cell is erased
- **THEN** the change count does not move

### Requirement: The grab dead zone is stated per axis
Because grab and smudge resample nearest-cell, the rounding SHALL be documented as PER AXIS: a displacement whose length exceeds half a cell can still round to zero on every axis and move nothing. Half a cell on the largest component SHALL be documented as a necessary and not a sufficient condition, because the falloff shrinks the pull away from the brush centre and the front-only gate, which is half weight on the plane through the centre, doubles the dead zone there.

#### Scenario: A drag shorter than a cell on every axis is dead
- **WHEN** a grab is applied with 0.4 of a cell on each of the three axes, a pull 0.69 of a cell long
- **THEN** every cell resolves to itself and nothing moves

#### Scenario: front_only widens the dead zone
- **WHEN** a displacement that moves material with the gate off is applied with the front-only gate on
- **THEN** it may move nothing, because the gate halves the pull at the brush centre
