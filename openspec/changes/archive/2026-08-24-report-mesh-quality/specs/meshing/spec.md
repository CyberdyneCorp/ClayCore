# meshing

## MODIFIED Requirements

### Requirement: Mesh validation
The module SHALL provide validation primitives: watertightness, 2-manifoldness, orientation consistency, degenerate- and sliver-triangle detection, boundary- and non-manifold-edge counts, the Euler characteristic, and sampled self-intersection checks. These back both CI export gates and any consumer's "clean geometry" claims.

Every one of those quantities SHALL be reachable by a consumer of the library, not only by code inside this repository. A validation primitive that only the repository's own tests can invoke does not satisfy this requirement, because the requirement's stated purpose is a consumer's claim about its geometry.

The sampled self-intersection pass SHALL be invocable with an explicit cap by any consumer, and a report SHALL make clear whether that pass ran. The derived "clean" predicate treats zero intersecting pairs as clean, so a report that cannot distinguish an unrun pass from a clean one would let "clean" mean two different things.

The module SHALL also provide the signed volume and the surface area of a mesh, and both SHALL be reachable by a consumer. The signed volume SHALL be positive for outward-facing normals, which is what locks the orientation convention.

#### Scenario: Validator catches a hole
- **WHEN** a mesh with one deleted triangle is validated
- **THEN** the watertight check fails and the report names the number of open boundary edges

#### Scenario: Self-intersection is checked when asked for
- **WHEN** a consumer validates a mesh with a non-zero self-intersection cap
- **THEN** spatially close, non-adjacent triangle pairs are tested exactly, up to that cap, and the count of hits is reported

#### Scenario: An unrun pass is not a clean result
- **WHEN** a mesh is validated with a zero self-intersection cap
- **THEN** the report shows that the pass did not run, so a consumer does not read the zero count as evidence of no self-intersection

#### Scenario: Volume locks the orientation convention
- **WHEN** the signed volume of a closed, outward-oriented mesh is computed
- **THEN** it is positive, and reversing the winding negates it
