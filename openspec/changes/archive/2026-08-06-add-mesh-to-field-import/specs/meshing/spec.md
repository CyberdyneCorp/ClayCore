# meshing — importing a mesh as a field

Delta for `add-mesh-to-field-import`.

## ADDED Requirements

### Requirement: A mesh can be queried for distance and insideness
The library SHALL provide an acceleration structure over a mesh's triangles answering two queries: the distance to the nearest point on the surface, and whether a point is inside it.

The structure SHALL be built once and queried many times, since a narrow-band sampling makes tens of thousands of queries against the same mesh.

#### Scenario: Distance matches an analytic shape
- **WHEN** a tessellated sphere is queried at points inside and outside it
- **THEN** the reported distances match the analytic sphere within the tessellation error

#### Scenario: Insideness is right for a closed mesh
- **WHEN** a closed mesh is queried at points plainly inside and plainly outside it
- **THEN** the answers are inside and outside respectively

### Requirement: The sign survives meshes that are not watertight
Insideness SHALL be determined by the generalized winding number, so that it degrades continuously on input that is not a clean closed surface rather than failing catastrophically.

A mesh with a hole SHALL still be signed sensibly away from the hole. A mesh whose triangles wind inconsistently, or which intersects itself, SHALL still produce a usable field rather than a field with inverted regions.

#### Scenario: A mesh with a hole is still signed
- **WHEN** a closed mesh has some of its triangles removed and is queried well away from the opening
- **THEN** points inside are still reported inside, and points outside are still reported outside

#### Scenario: Winding direction does not decide the answer
- **WHEN** a mesh's triangle winding is reversed throughout
- **THEN** the surface it describes is unchanged in position, and the import reports a field of the same shape

#### Scenario: A self-intersecting mesh does not invert
- **WHEN** two overlapping closed shapes are imported as one mesh
- **THEN** the region inside either of them is reported inside, rather than the overlap being reported outside

### Requirement: Distant geometry is summarized rather than visited
Summing a solid angle per triangle is linear in the mesh, which a narrow-band sampling cannot afford. Nodes far enough from the query point SHALL be summarized by an aggregate term instead of being descended, while nodes near it SHALL be descended so the answer near the surface is exact.

#### Scenario: The approximation agrees with the exact sum
- **WHEN** the same points are evaluated with summarization enabled and by summing every triangle
- **THEN** the two agree on which side of the surface each point is on

#### Scenario: Import does not scale with the triangle count the naive way
- **WHEN** the same shape is imported at a low and at a high tessellation
- **THEN** the time taken grows far more slowly than the triangle count

### Requirement: A mesh can be sampled into a field
The library SHALL sample a mesh into a sparse narrow-band volume, choosing the sampled region from the mesh's own bounds unless told otherwise, so that an imported model becomes an ordinary item that can be combined, cut and sculpted.

#### Scenario: An imported mesh becomes a usable item
- **WHEN** a mesh is imported as a field and subtracted from a box
- **THEN** the result is the box with the mesh's shape removed

#### Scenario: Re-meshing the field returns the shape
- **WHEN** a mesh is imported as a field and that field is meshed again
- **THEN** the result occupies the same space as the original within the sampling tolerance

#### Scenario: An empty mesh is refused rather than sampled
- **WHEN** a mesh with no triangles is imported
- **THEN** the call fails rather than producing a volume that reads unwritten data

### Requirement: A mesh can be brought in from a file or from memory
The library SHALL load a mesh by extension, as the counterpart to saving one, and SHALL build a mesh from caller-supplied vertices and triangle indices. Without either, the import has nothing to import.

An index pointing past the vertices SHALL be refused or dropped rather than read.

#### Scenario: A saved mesh loads again
- **WHEN** a mesh is saved and then loaded from the same path
- **THEN** it has the same triangles

#### Scenario: An unknown extension is reported
- **WHEN** a mesh is loaded from a path whose extension no loader handles
- **THEN** the call reports that rather than guessing at a format

#### Scenario: Indices are checked against the vertices
- **WHEN** a mesh is built from indices that point past the end of the vertex array
- **THEN** the call fails, or the offending triangle is dropped, rather than reading past the buffer
