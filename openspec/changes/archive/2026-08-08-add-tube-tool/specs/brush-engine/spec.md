# brush-engine — the Tube tool

Delta for `add-tube-tool`.

## ADDED Requirements

### Requirement: A drawn path resolves into a tube
The library SHALL resolve a path of control points and a handful of settings into an ordinary edit item describing a tube along that path — a rope, pipe, tentacle or hair strand. Resolving SHALL be pure: no document is read or touched, so a caller can preview a tube before committing it.

The result SHALL be an ordinary item, so undo, serialization, picking, masking and meshing apply to it unchanged.

#### Scenario: A path becomes a tube
- **WHEN** a path of control points is resolved
- **THEN** the result is an item whose surface follows that path at the requested radius

#### Scenario: It is an ordinary item
- **WHEN** a resolved tube is added to a layer
- **THEN** it combines, saves, picks and meshes exactly as any other item does

### Requirement: The radius may vary along the tube
The radius SHALL be settable at the start, the middle and the end, and interpolated between them by ARC LENGTH so that a path whose control points bunch does not bunch the taper.

#### Scenario: A tapered tube
- **WHEN** a tube is resolved with different radii at its start and end
- **THEN** its thickness changes along its length, reaching each radius where that radius was asked for

#### Scenario: A uniform tube
- **WHEN** all three radii are equal
- **THEN** the thickness is constant along the tube

#### Scenario: The taper follows arc length, not point index
- **WHEN** the same path is resolved with its control points evenly spaced, and again with them bunched at one end
- **THEN** the radius at a given fraction of the length is the same in both

### Requirement: The cross-section decides the representation
A tube with no profile SHALL be a swept sphere, which is an EXACT distance field. A tube with a profile SHALL be a swept item, which is a bound field and costs safe step scale.

This is chosen by whether a profile is given rather than exposed as a separate flag, since a caller asking for a square cross-section has already said which one it wants.

#### Scenario: A round tube stays exact
- **WHEN** a tube with no profile is compiled
- **THEN** the document reports the field as exact and the safe step scale is 1

#### Scenario: A profiled tube declares its cost
- **WHEN** a tube with a profile is compiled
- **THEN** the field reports as inexact and the safe step scale is below 1

### Requirement: Smoothness is the point type
Whether the tube runs smoothly through its control points or turns sharply at them SHALL be the curve's existing point type rather than a separate toggle, so a tube's path is the same kind of curve every other item takes.

#### Scenario: Sharp against smooth
- **WHEN** the same points are resolved with hard points and with B-spline points
- **THEN** the two fields differ, and the B-spline one passes further from the corner

#### Scenario: A closed tube joins
- **WHEN** a tube is resolved closed
- **THEN** its last point joins back to its first, with no cap between them

### Requirement: A path that is not one is refused
Fewer points than describe a path, or a radius that is not positive anywhere, SHALL be refused rather than yielding an item that contributes nothing.

#### Scenario: Degenerate input
- **WHEN** a tube is asked for from a single point, or with every radius zero
- **THEN** it is refused
