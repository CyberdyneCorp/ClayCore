# meshing — an exact per-face grouping

Delta for `native-mesh-polygroups`.

## ADDED Requirements

### Requirement: A mesh may carry an exact per-face grouping

A mesh representation MAY carry a grouping of its own faces, distinct from the
representation-independent spatial grouping a document carries.

The two SHALL NOT be conflated. A spatial grouping answers "which group is this
point in" for any representation and quantises its border to a lattice; a
per-face grouping answers "which group is this face in" and its border is an
edge set. Conversion between them MAY be offered and SHALL declare what it
loses: a per-face grouping rasterised into a lattice loses the exact border, and
a lattice sampled onto faces gains a quantised one. Round-trip exactness SHALL
NOT be promised across the sampled representation.

The grouping unit SHALL be the face as the mesh's index buffer presents it.
Where a mesh additionally carries logical quads, both triangles of a quad SHALL
carry the same group — the quad array is optional and is cleared by any
operation that rewrites the indices, so a unit defined on it would not survive
subdivision or conversion to a dynamic surface.

A mesh that carries no grouping SHALL cost nothing for the feature: no storage,
and no lookup on any path that does not ask for one.

Membership SHALL NOT imply a constraint. Whether an operation may cross a group
boundary SHALL be a property of that operation, requested explicitly, so that
the topology primitives stay general and a host chooses its own default.

Group identity SHALL be numeric and stable. Display names and colours belong to
the host.

Deleting a group SHALL reassign its faces to the default group and SHALL NOT
delete faces, on the same reading the spatial grouping already gives that word.

#### Scenario: A mesh with no grouping is unchanged
- **WHEN** a mesh that was never grouped is sculpted, subdivided or converted
- **THEN** it carries no grouping storage and no operation consults one

#### Scenario: A quad's triangles agree
- **WHEN** a mesh carrying logical quads is grouped
- **THEN** both triangles of every quad carry the same group

#### Scenario: Crossing a boundary is asked for, not assumed
- **WHEN** an operation that could cross a group boundary is invoked without a policy
- **THEN** it behaves as it did before groups existed

#### Scenario: Conversion states its loss
- **WHEN** a per-face grouping is converted to a spatial one, or the reverse
- **THEN** the conversion is explicit and its loss is declared rather than discovered
