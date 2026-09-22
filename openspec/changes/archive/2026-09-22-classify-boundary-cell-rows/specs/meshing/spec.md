## ADDED Requirements

### Requirement: Exact row classification for boundary cells
Boundary enumeration SHALL emit exactly the unrequested owner's cells whose closed boxes touch a requested neighboring brick, in the existing z/y/x order.

#### Scenario: First, interior and last planes
- GIVEN a positive brick dimension greater than one and any requested-neighbor mask
- WHEN row categories are reused
- THEN emitted coordinates SHALL match independent closed-box intersection enumeration without duplicates

#### Scenario: One-cell brick
- GIVEN a brick dimension of one
- WHEN one or more neighboring bricks are requested
- THEN its single cell SHALL be emitted exactly once

#### Scenario: Complete mesh replay
- GIVEN full or subset brick requests, including boundary triangles
- WHEN row classification replaces per-cell neighbor tests
- THEN complete mesh attributes, indices and brick ranges SHALL remain unchanged
