# c-abi — one-sided flatten

Delta for `add-flatten-modes`.

## ADDED Requirements

### Requirement: Flatten mode across the C ABI
The flatten descriptor SHALL carry the mode, appended so that a caller compiled against the previous layout still describes a two-sided flatten and still works.

#### Scenario: An older descriptor still means two-sided
- **WHEN** a caller passes a descriptor sized to the layout that predates the mode
- **THEN** the call succeeds and performs a two-sided flatten

#### Scenario: An unknown mode is refused
- **WHEN** a descriptor names a mode the ABI does not define
- **THEN** it is refused rather than silently treated as two-sided
