# python-bindings — sweeping along a guide

Delta for `add-swept-n`.

## ADDED Requirements

### Requirement: Sweeps from Python
The module SHALL expose a swept item taking guide control points with their types and tolerance, and two or more profiles.

#### Scenario: Sweeping a circle along a curve
- **WHEN** a script sweeps a circle along a spline guide
- **THEN** the field is material along the guide and empty away from it
