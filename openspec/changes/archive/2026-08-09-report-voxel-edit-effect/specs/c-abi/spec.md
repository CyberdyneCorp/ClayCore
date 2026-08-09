# c-abi — an edit's effect is observable

Delta for `report-voxel-edit-effect`.

## ADDED Requirements

### Requirement: A voxel edit's effect is readable across the ABI
The ABI SHALL expose the grid's change count as a query alongside the occupied count, taking a `uint64_t` out-parameter rather than a `size_t` because the counter is never reset and a 32-bit host would wrap it in a long session. A NULL out-parameter SHALL be tolerated, matching the other grid queries; a NULL grid SHALL be refused with an invalid-argument error.

The header SHALL state why it exists — sub-cell drags and other effect-free edits are legal and common, and neither the result code nor the occupied count can distinguish them — what it counts, that it is monotone and meaningful only as a difference, the pinch/magnify upper bound, and that its unit is cells changed and NOT the stamps-run or items-warped unit of the `out_applied` parameters elsewhere in the header.

This SHALL be a single grid-level query rather than a per-verb sibling: an entry point differing from an existing one only by an extra out-parameter would be two ways to say one thing, and covering the verbs that share the blind spot would take eleven of them plus one per verb added later.

#### Scenario: A drag that reached nothing is distinguishable from one that did
- **WHEN** a host reads the change count, applies a sub-cell grab, reads it again, applies a supra-cell grab and reads it a third time
- **THEN** the first difference is zero, the second is non-zero, and both calls returned success

#### Scenario: The counter agrees with the engine
- **WHEN** the same sequence of edits is applied across the ABI and directly on the engine's grid
- **THEN** the count read across the ABI equals the engine's

#### Scenario: A NULL grid is refused, a NULL out-parameter is not
- **WHEN** the query is called with a NULL grid, and again with a valid grid and a NULL out-parameter
- **THEN** the first is refused with an invalid-argument error and the second succeeds

### Requirement: A valid edit with no effect stays CLAY_OK
An entry point whose call was well-formed and whose effect was nothing SHALL return `CLAY_OK`. No `clay_result` value SHALL be added to mean "valid but had no effect": the enum is ABI-stable, and an existing entry point returning a new non-zero value would make every already-compiled caller treat a success as a failure. An outcome that a host wants to observe SHALL arrive through an out-parameter or a query, on the same footing as a rejected brick submission and as "nothing to undo".

The sculpting-verbs section of the header SHALL state this once for all of them, rather than leaving each verb to imply it: a flatten on an already-flat region, a sub-cell smudge, a dithered stamp that misses every cell and a footprint over empty space are all ordinary successes.

#### Scenario: A sub-cell grab succeeds
- **WHEN** a grab crosses the ABI with a displacement shorter than half a cell on every axis
- **THEN** the call returns `CLAY_OK` and the grid is unchanged

#### Scenario: No new result code appears
- **WHEN** the result enum is compared against the previous ABI minor
- **THEN** it holds the same values, so a caller compiled against the older header still classifies every outcome as it did
