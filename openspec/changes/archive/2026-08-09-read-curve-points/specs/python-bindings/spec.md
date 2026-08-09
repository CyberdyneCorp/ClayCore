# python-bindings — Read Curve Points

Delta for `read-curve-points`.

## MODIFIED Requirements

### Requirement: Sweeps from Python
The module SHALL expose a swept item taking guide control points with their types and tolerance, and two or more profiles. A PLACED sweep's guide SHALL be editable through the same point-list replace a curve uses, since a guide is an ordinary curve; the replace SHALL refuse what the constructor refuses — closing a guide, which the constructor offers no flag for at all, and leaving one with fewer than two points.

#### Scenario: Sweeping a circle along a curve
- **WHEN** a script sweeps a circle along a spline guide
- **THEN** the field is material along the guide and empty away from it

#### Scenario: A placed sweep's guide is reshaped
- **WHEN** a script replaces a placed sweep's points with a differently bent guide
- **THEN** the field follows the new guide

#### Scenario: A guide cannot be closed after the fact
- **WHEN** the replace asks for a closed guide
- **THEN** it is refused with the reason, and the sweep is unchanged

#### Scenario: A guide cannot be cut below two points
- **WHEN** the replace hands a placed sweep a single-point guide
- **THEN** it is refused with the reason, and the sweep still follows its guide
