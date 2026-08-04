# python-bindings — profiles and lifts

Delta for `add-profile-lifts`.

## ADDED Requirements

### Requirement: Profile primitives from Python
The module SHALL expose profile objects (`Circle2`, `Box2`, `Hexagon2`, `Triangle2`, `Trapezoid2`, `Vesica2`, `Polygon`) and the lifts that consume them (`Extrude(profile, half_depth)`, `Revolve(profile, offset)`), with `Polygon` accepting an `(N,2)` float32 array or a sequence of pairs. Profiles SHALL survive a `.clayspace` round trip, and lifting an unsupported open curve SHALL raise a clear error naming the flattening workaround.

#### Scenario: Extruded polygon from numpy
- **WHEN** a script extrudes a polygon given as an `(N,2)` array and meshes the document
- **THEN** the mesh is watertight and its cross-section matches the polygon

#### Scenario: Revolved profile builds a torus
- **WHEN** a circle profile of radius r is revolved at offset R
- **THEN** the field matches `clay.Torus(R=R, r=r)` within tolerance
