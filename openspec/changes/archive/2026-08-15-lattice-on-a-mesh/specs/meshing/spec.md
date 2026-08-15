# meshing — a lattice cage over a mesh layer

Delta for `lattice-on-a-mesh`.

## ADDED Requirements

### Requirement: A lattice cage deforms a mesh layer's vertices
The meshing capability SHALL provide a lattice (free-form deformation) cage that moves a mesh's vertices, so an artist can reshape a whole form by dragging a few control points rather than by brushing.

It SHALL run FORWARD — each vertex's parametric position in the cage is found and the basis evaluated to place it — and SHALL NOT invert anything. A mesh already knows where its vertices are; inversion is a property of evaluating an implicit field, which is why the SDF form of this feature is a different and harder problem. This is what ZBrush's Gizmo Lattice and Blender's Lattice modifier do.

Topology SHALL NOT change. `indices` and `quads` SHALL come out byte for byte as they went in, exactly as every other verb on a mesh layer guarantees.

The cage SHALL store control-point OFFSETS rather than positions, so that a cage nobody has touched is EXACTLY the identity at every point, with no special case. The warp is the point plus the offset field evaluated at its parameters.

Evaluation SHALL be trivariate Bernstein, one formula for every cage size, with degree one less than the control-point count on each axis — so a 2x2x2 cage is exactly trilinear and needs no separate path. Bernstein interpolates the corner control points, so dragging a corner moves that corner of the box exactly.

The cage SHALL take a free resolution per axis, at least two. The fixed 3x3x3 that was proposed for the SDF form was a consequence of the tape record's slot budget, which a mesh lattice does not have. The cost SHALL be stated: Bernstein support is global per axis, so on a large cage one control point still moves everything a little.

A vertex OUTSIDE the cage's box SHALL travel rigidly with the nearest point of the cage rather than being drawn onto it, by clamping its parameters and applying the offset there. Evaluating the cage at a clamped parameter and using the RESULT rather than the OFFSET would collapse every outside vertex onto the box, which is the mistake this convention exists to avoid. It is the same "held beyond it" behaviour the ranged twist and bend already use.

Applying a lattice SHALL record into the same vertex-delta record the brushes produce, so it is one undo step, and SHALL recompute the normals of the vertices it moved.

It SHALL be reachable from `pyclay` and the C ABI.

#### Scenario: An untouched cage changes nothing
- **WHEN** a lattice whose offsets are all zero is applied to a mesh
- **THEN** every vertex position and normal is bit-identical to what it was

#### Scenario: Moving every control point translates the mesh
- **WHEN** every control point of a cage is offset by the same vector
- **THEN** every vertex inside the box moves by that vector

#### Scenario: A two-per-axis cage is trilinear
- **WHEN** a 2x2x2 cage is applied
- **THEN** the displacement at any point is the trilinear blend of its eight corner offsets

#### Scenario: Material outside the box travels rigidly
- **WHEN** a vertex lies outside the cage's box and the cage is deformed
- **THEN** it moves by the offset at the nearest point of the cage, and is not drawn onto the box

#### Scenario: Topology survives
- **WHEN** any lattice is applied to a quad or triangle mesh
- **THEN** `indices` and `quads` are unchanged, byte for byte

#### Scenario: A lattice is one undo step
- **WHEN** a lattice is applied with a delta record and then reverted
- **THEN** the mesh is bit-identical to before, positions and normals alike

### Requirement: The SDF lattice remains open, and says so
The absence of a lattice on SDF ITEMS SHALL remain visible in the documentation rather than being read as closed by the mesh-layer feature.

A claycore SDF deformer is an inverse point map and forward FFD has no closed-form inverse, so that form needs a compromise this one does not. Documenting the distinction is what stops a reader concluding the gap is filled.

#### Scenario: The remaining gap is stated
- **WHEN** a reader consults the ZBrush-equivalence table
- **THEN** the Gizmo Lattice is recorded as available on mesh layers and still absent on SDF items, with the reason
