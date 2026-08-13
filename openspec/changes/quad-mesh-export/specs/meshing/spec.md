# meshing — a mesh can carry the quads its mesher built

Delta for `quad-mesh-export`.

## ADDED Requirements

### Requirement: A mesh carries an optional quad list beside its triangles
`mesh::Mesh` SHALL carry an OPTIONAL quad index array, four indices per quad, empty on every mesh produced today.

When the quad array is non-empty it SHALL be accompanied by its own triangulation in `indices`: quad `q` with corners `(a, b, c, d)` SHALL appear as triangles `(a, b, c)` and `(a, c, d)` at `indices[6q .. 6q+5]`, over the same positions. `indices.size()` SHALL therefore equal `quads.size() / 4 * 6`.

This invariant is the reason the array is additive rather than a replacement. A consumer that ignores the quad array SHALL see a complete and correct triangle mesh, so decimation, the BVH, validation, every exporter, the C accessors and the mesh stream SHALL keep producing byte-identical results for every mesh in existence today without being modified.

No mesher SHALL begin filling the quad array on its own. Every quad-producing path SHALL be a distinct entry point the caller chooses, because a mesh that silently grew a third index array would change what every existing call allocates and what every saved document weighs.

The module SHALL provide a consistency check for the invariant, so the boundary that reads a mesh from bytes and the tests that assert the invariant do not each restate it.

Any operation that rewrites `indices` SHALL clear the quad array rather than leave it describing triangles that no longer exist. Decimation SHALL clear it. An operation that moves positions without touching indices SHALL keep it. Concatenation SHALL carry quads only when EVERY input carries them, rebasing as it rebases the triangles, and SHALL drop them otherwise — the all-or-nothing rule already applied to normals, colours and uvs.

#### Scenario: A triangle mesh is unchanged
- **WHEN** any mesher that existed before this change runs
- **THEN** the mesh it returns has an empty quad array and identical positions, attributes and indices to what it returned before

#### Scenario: The triangles are the quads
- **WHEN** a quad mesh is produced
- **THEN** its index list is exactly the two-triangle expansion of its quad list, in order, and the consistency check passes

#### Scenario: Decimation drops the quads
- **WHEN** a quad mesh is decimated
- **THEN** the result carries no quads, and its triangles are what decimation would have produced for the same mesh without them

#### Scenario: Concatenating a quad mesh with a triangle mesh drops the quads
- **WHEN** a quad mesh and a triangle mesh are concatenated
- **THEN** the result carries no quads and its triangles are the concatenation, as before

### Requirement: The lattice dual is the quad mesher
The module SHALL provide a quad mesher for tapes built on the LATTICE DUAL — the surface-nets construction already in `dual_grid_mesh`, one vertex per sign-changing cell and one quad per sign-changing lattice edge — retaining the four corners it already computes.

The dual is chosen over greedy merging because merged rectangles create T-JUNCTIONS where one long quad abuts several short ones, and a T-junction cracks under subdivision and splits normals. The dual's interior vertices have valence four and no vertex lands mid-edge.

The mesher SHALL NOT change the diagonal on which it triangulates. It triangulates on the 0–2 diagonal today; a non-planar quad would triangulate better on its shorter one, and switching would change the triangles every existing caller of the dual meshers receives.

Two properties of the output SHALL be stated in the header rather than discovered:

- **The quads are NOT planar.** Four cell vertices around a lattice edge are placed independently and nothing makes them coplanar, so a consumer that triangulates on its own terms may shade a face differently than this library's triangulation does. No planarisation is performed: flattening a face moves vertices off the surface, trading a shading artifact for a geometric error.
- **The output is NOT manifold and NOT watertight**, for the reason the surface-nets header already gives — a cell the surface crosses twice gets a single vertex and pinches the sheets. The marching mesher remains the watertight, 2-manifold export path and is unaffected.

#### Scenario: A quad-meshed sphere is quads all the way
- **WHEN** a sphere tape is quad-meshed
- **THEN** every face in the output is a quad, every interior vertex is shared by four of them, and no vertex lies in the interior of another face's edge

#### Scenario: The quad mesh is the dual mesh
- **WHEN** a tape is meshed with the dual mesher and quad-meshed at the same cell size
- **THEN** the positions and the triangle indices are identical, and only the quad array differs

### Requirement: A quad count is a target the mesher approaches, not one it hits
The module SHALL accept a target quad count and search for the lattice cell size that comes closest to it, reporting the cell size it settled on, the count it actually produced, how many iterations it took, and whether it landed inside the requested tolerance.

**The target is a HINT with a reported actual — not a ceiling and not an exact count.** A ceiling SHALL NOT be promised, because quad count is not monotonic in cell size: a finer lattice can resolve a thin feature the coarser one missed entirely and so ADD area and quads. Where two candidates are equally close, the search SHALL prefer the one that does not exceed the target.

Because count scales as the inverse square of cell size, a one percent change in cell size moves the count about two percent. The documented expectation SHALL be a landing inside roughly five to ten percent of a target within the default iteration cap; the default tolerance SHALL be ten percent, and a tolerance far below that SHALL be expected to exhaust the cap and return the best attempt with the tolerance flag clear, rather than iterating without bound.

Each iteration is a full mesh, including a full dense field evaluation on the tape path. The iteration cap SHALL therefore be a caller-visible cost knob with a small default, and the header SHALL say so.

The search SHALL clamp rather than fail:

- It SHALL never request a lattice the mesher's own resolution pricing would refuse; it stops at that ceiling and reports that it clamped.
- When the shape's own topology collapses before the target is reached, it SHALL return its best result and report that it did not converge, rather than returning the collapse as the answer.

Decimation SHALL NOT be used to approach a target: quadric edge collapse is a triangle operation and breaks the quad pairing on its first collapse.

#### Scenario: A target is approached and the actual is reported
- **WHEN** a shape is quad-meshed with a target of twenty thousand quads at the default tolerance
- **THEN** the returned mesh's quad count is within ten percent of twenty thousand, and the report states that count, the cell size used and that it converged

#### Scenario: An unreachable target reports rather than lies
- **WHEN** a target is requested that the resolution ceiling cannot reach
- **THEN** the mesher returns the mesh at the finest lattice it is allowed, and the report states the actual count, the cell size, and that the search clamped

#### Scenario: An explicit cell size skips the search
- **WHEN** a cell size is given and no target
- **THEN** exactly one mesh is produced at that cell size and the report echoes it with zero iterations

### Requirement: The quad mesher is a lattice grid, not retopology
Every place this feature is described — the module header, the C header, the Python docstrings, the example and the documentation — SHALL state plainly that the output is a REGULAR QUAD GRID DERIVED FROM A SAMPLING LATTICE and is NOT field-aligned retopology.

Specifically it SHALL state that the quads follow the lattice rather than the form: no edge loops follow a limb or a mouth, no poles are placed at features, density does not follow curvature, and the result is NOT animation-ready. It SHALL state that this is the input a retopology pass replaces rather than the output one produces.

A user who reaches for this expecting a quad remesher's output SHALL learn that from the documentation, not from the result.

#### Scenario: The claim is where a user will read it
- **WHEN** the quad meshing header, the C entry points, the Python docstrings and the example are read
- **THEN** each one states that this is a lattice-derived quad grid and not field-aligned retopology, and none of them describes the output as retopology, as animation-ready or as edge-loop-following
