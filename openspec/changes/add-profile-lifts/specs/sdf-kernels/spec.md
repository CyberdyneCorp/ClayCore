# sdf-kernels — profiles and lifts reachable from a document

Delta for `add-profile-lifts`.

## MODIFIED Requirements

### Requirement: 2D profile set
`prim2d.h` SHALL provide exact 2D SDFs for extrude/revolve (01 §1.3): circle, box, segment, hexagon, equilateral triangle, trapezoid, vesica, arbitrary polygon (exact, even-odd sign rule), and quadratic Bézier. Cubic Bézier SHALL be evaluated by adaptive quadratic subdivision, never by quintic root-finding.

The closed profiles — circle, box, hexagon, equilateral triangle, trapezoid, vesica, and the arbitrary polygon — SHALL additionally be expressible in the tape as the profile of a lift, with polygon vertices carried out-of-line. Open curves (segment, Bézier) remain header-only because they are unsigned distances rather than regions; documents reach curved outlines by flattening them to a polygon.

#### Scenario: Polygon profile handles concavity
- **WHEN** a concave polygon profile is evaluated at points inside and outside concave regions
- **THEN** the sign follows the even-odd rule and the distance is exact to the nearest edge

#### Scenario: Profile reachable through a lift
- **WHEN** a document contains an item whose primitive is an extrusion of a polygon profile
- **THEN** the compiled tape evaluates it identically to applying `cop_extrude` to `sd_polygon2` directly

### Requirement: Lifts
`lift.h` SHALL provide exact extrusion and exact revolution of exact 2D profiles (01 §2.6), and extrude-to/loft flagged as bound.

Extrusion and revolution SHALL be expressible in the tape as primitive opcodes carrying a profile, so a document can build profile-driven shapes. Loft remains header-only until an item can carry two profiles.

#### Scenario: Revolve preserves exactness
- **WHEN** an exact 2D profile is revolved
- **THEN** the resulting 3D field is exact and the tree exactness state records `exact`

#### Scenario: Lifted items evaluate through the tape
- **WHEN** a circle profile is extruded and, separately, revolved in a document
- **THEN** the fields equal a capped cylinder and a torus respectively, within meshing tolerance

#### Scenario: Lifted items keep tracked exactness
- **WHEN** a tape containing only extrusions and revolutions of exact profiles is compiled
- **THEN** its field info remains exact and its safe step scale stays 1
