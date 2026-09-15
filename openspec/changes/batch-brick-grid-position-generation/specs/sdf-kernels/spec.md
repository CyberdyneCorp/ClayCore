## ADDED Requirements

### Requirement: Bulk brick coordinates preserve scalar positions
A bulk source-coordinate fill SHALL produce the same float bits and sample order as calling BrickGrid::sample_position for every requested sample. It SHALL preserve halo coordinates and global-cell arithmetic while deriving each brick base only once.

#### Scenario: Consecutive brick window
- **WHEN** a fill requests consecutive bricks beginning at a nonzero slot, including row or plane crossings
- **THEN** all output coordinate bits match the scalar reference in x-fastest sample order
- **AND** only the requested output range is written

#### Scenario: Empty window
- **WHEN** the requested brick count is zero
- **THEN** no output memory is accessed

#### Scenario: Smooth source preparation
- **WHEN** a Smooth transaction materializes source samples through the bulk coordinate writer
- **THEN** field samples and preview delta semantics remain unchanged
