# examples — the gallery shows what quad export is and is not

Delta for `quad-mesh-export`.

## ADDED Requirements

### Requirement: An example exports quads from both sources
The gallery SHALL carry an example that quad-meshes an SDF document AND a voxel sculpt, writes OBJ, PLY and FBX, and prints for each the quad count actually produced against the count requested.

It SHALL also write GLB and print that it came out as triangles because glTF 2.0 has no quad primitive mode, so the one surprising outcome of this feature appears in the gallery output rather than in a bug report.

Its header comment SHALL state that the output is a REGULAR QUAD GRID DERIVED FROM A LATTICE and NOT field-aligned retopology — no edge loops following the form, nothing animation-ready — and SHALL say what a user should reach for a quad remesher for instead.

#### Scenario: The example runs and reports
- **WHEN** the quad export example runs
- **THEN** it exits zero, writes its OBJ, PLY, FBX and GLB under the gallery's output directory, and prints for each mesh the requested count and the count actually produced

#### Scenario: The honesty statement is in the example
- **WHEN** the example's header is read
- **THEN** it states that this is a lattice-derived quad grid rather than field-aligned retopology, and does not describe the result as animation-ready
