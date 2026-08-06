# scene-model — a swept item

Delta for `add-swept-n`.

## ADDED Requirements

### Requirement: A swept item carries a guide and profiles
A swept item SHALL carry a guide as control points with the same types, handles and tolerance a curve item uses, and SHALL carry its profiles in the same list a loft uses. A guide SHALL NOT be a new kind of curve.

#### Scenario: A sweep round trips
- **WHEN** a document containing a sweep with a spline guide and three profiles is saved and reloaded
- **THEN** the guide's control points and types, and every profile, come back, and the field is unchanged

#### Scenario: The guide honours its point types
- **WHEN** the same guide points are given hard and then spline types
- **THEN** the swept shapes differ
