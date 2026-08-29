# scene-model — memory categories for the surface tier

Delta for `add-extreme-poly-runtime`.

## ADDED Requirements

### Requirement: The roll-up covers the surface representations
The document memory roll-up SHALL account for adaptive surfaces, multiresolution hierarchies and sculpt layers, each split into authoritative content and rebuildable cache, alongside the categories it already reports.

A category that is missing from the roll-up is invisible to a host answering a memory warning, and the roll-up already found one such omission — a node accounting six members behind the type it walked, missing exactly the largest things a node owns.

The roll-up SHALL be checked against the types it walks by a test that fails when a new member is added without being accounted, rather than by review.

#### Scenario: A new surface category is accounted
- **WHEN** a document holding every surface representation is measured
- **THEN** each representation contributes to the report, and the sum of the categories equals the reported total

#### Scenario: An unaccounted member fails the build
- **WHEN** a member is added to an accounted type without being included in its byte count
- **THEN** the accounting test fails
