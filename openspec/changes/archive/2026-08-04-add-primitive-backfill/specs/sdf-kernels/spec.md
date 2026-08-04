# sdf-kernels — every primitive reachable from a document

Delta for `add-primitive-backfill`.

## MODIFIED Requirements

### Requirement: 3D primitive set
`prim3d.h` SHALL provide exact signed distance functions (01 §1.1) for: sphere, box, rounded box, box frame, torus, capped torus, link, capsule, infinite cylinder, capped cylinder (incl. arbitrary-axis), rounded cylinder, cone (exact), capped cone, round cone, plane, hexagonal prism, octahedron, pyramid, cut sphere, cut hollow sphere, solid angle, tetrahedron, and platonic solids via plane folds. It SHALL additionally provide bound-only primitives (01 §1.2): ellipsoid, triangular prism, cheap octahedron, superellipsoid / L-norm sphere — each flagged as `bound`.

Every one of these SHALL be expressible in the tape, so a document can use the whole set. Primitives with no finite extent (plane, infinite cylinder) SHALL make their item report infinite influence, and bound-only primitives SHALL downgrade the tape's tracked exactness.

#### Scenario: Exact primitive returns true distance
- **WHEN** an exact primitive is evaluated at a point at known geometric distance d from its surface
- **THEN** the returned value equals ±d (sign by inside/outside) within 1e-6 absolute for unit-scale shapes

#### Scenario: Bound primitive is conservative
- **WHEN** a bound-only primitive is evaluated at any sample point in a property test
- **THEN** the returned value SHALL NOT exceed the true distance to the surface (|f(p)| ≤ true distance, sign correct)

#### Scenario: Every primitive evaluates through the tape
- **WHEN** each primitive is placed in a document and the tape is evaluated
- **THEN** the result equals calling its kernel function directly with the same parameters

#### Scenario: Unbounded primitives are never culled
- **WHEN** an item using a plane or infinite cylinder is compiled for a brick far from the origin
- **THEN** the item appears in the culled tape and the band-clamped result matches the full tape

#### Scenario: Bound primitives downgrade exactness
- **WHEN** a tape contains a bound-only primitive such as the cheap octahedron
- **THEN** its field info is non-exact and stepping by its safe step scale never crosses the surface
