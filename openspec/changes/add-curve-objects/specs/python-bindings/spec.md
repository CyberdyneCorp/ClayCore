# python-bindings — curves

Delta for `add-curve-objects`.

## ADDED Requirements

### Requirement: Curves from Python
The module SHALL expose a curve constructor taking control points with per-point radius and type, optional Bezier handles, a closed flag and a tolerance, and SHALL expose replacing an existing item's points.

#### Scenario: Authoring a curve
- **WHEN** a script builds a closed spline curve and evaluates the document
- **THEN** the field is a smooth tube through the control points

#### Scenario: Editing a curve
- **WHEN** a script replaces a placed curve's points
- **THEN** the field changes, and undoing restores it
