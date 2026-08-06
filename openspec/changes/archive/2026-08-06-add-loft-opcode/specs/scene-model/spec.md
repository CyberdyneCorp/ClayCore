# scene-model — an item carrying profiles

Delta for `add-loft-opcode`.

## ADDED Requirements

### Requirement: An item may carry a list of profiles
An item SHALL be able to carry two or more 2D profiles, each with its own polygon vertices where it is a polygon profile. The single-profile lifts SHALL keep the field they already use, so no existing document changes meaning.

A loft with fewer than two profiles SHALL be refused rather than compiled into a degenerate shape.

#### Scenario: A loft round trips
- **WHEN** a document containing a loft of a circle and a polygon is saved and reloaded
- **THEN** every profile, its parameters and its vertices come back, and the field is unchanged

#### Scenario: Existing lifts are unaffected
- **WHEN** a document containing an extrusion is compiled before and after this change
- **THEN** the tape is identical

#### Scenario: A degenerate loft is refused
- **WHEN** a loft is built with one profile or none
- **THEN** it is refused
