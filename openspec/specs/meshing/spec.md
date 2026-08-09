# meshing Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Default mesher with watertight guarantee
`clay::mesh` SHALL provide a default cell-marching mesher whose output is watertight and 2-manifold by construction, running only over surface-crossing bricks. v1 implements this with marching tetrahedra (Freudenthal 6-tet decomposition with globally consistent face diagonals — no ambiguous configurations exist, so the guarantee is structural); a table-based marching cubes with asymptotic-decider ambiguity resolution MAY replace it later as a triangle-count optimization provided the same guarantees hold. The CPU implementation is the golden reference; GPU implementations (Metal/CUDA) SHALL match its topology invariants (watertight, manifold, Euler characteristic on golden scenes) though not bit-identical vertex positions.

#### Scenario: Watertight across the op matrix
- **WHEN** golden scenes covering every op × blend combination are meshed at standard resolutions
- **THEN** every output mesh passes watertight and 2-manifold validation

#### Scenario: GPU meshing topology parity
- **WHEN** a golden scene is meshed on CPU and on a GPU backend
- **THEN** both meshes are watertight/manifold with identical Euler characteristic

### Requirement: Surface nets preview mesher
The module SHALL provide a surface-nets mesher for cheap smooth preview meshes, sharing the brick traversal and attribute sampling of marching cubes.

#### Scenario: Preview mesh from bricks
- **WHEN** surface nets runs over a filled brick cache
- **THEN** it produces a valid mesh in less time than marching cubes at equal resolution (benchmarked, regression-gated)

### Requirement: Dual contouring (flagged)
The module SHALL provide dual contouring with QEF minimization over Hermite data (position + normal per edge crossing) for sharp-edge export, in its manifold variant, shipped behind an explicit opt-in flag until hardened post-v1.

#### Scenario: Sharp edge preserved
- **WHEN** a chamfered box union is meshed with dual contouring
- **THEN** the chamfer's edge lines appear as sharp polylines in the mesh (vertices placed by QEF on the edges), unlike the rounded MC result

### Requirement: Decimation
The module SHALL provide quadric edge-collapse decimation via meshoptimizer, driven by target triangle ratio or error bound, aware of vertex color attributes (collapses SHALL NOT merge across strong color boundaries beyond the configured attribute weight).

#### Scenario: Ratio-targeted decimation
- **WHEN** a mesh is decimated to ratio 0.5
- **THEN** the output has ≤ 50% of input triangles, remains watertight if the input was, and preserves color regions within the attribute error bound

### Requirement: Mesh validation
The module SHALL provide validation primitives: watertightness, 2-manifoldness, degenerate-triangle detection, and sampled self-intersection checks. These back both CI export gates and any consumer's "clean geometry" claims.

#### Scenario: Validator catches a hole
- **WHEN** a mesh with one deleted triangle is validated
- **THEN** the watertight check fails and reports the open edge loop

### Requirement: Mesh attributes
Meshers SHALL emit vertex colors sampled from the scene color field (faithful to blend gradients via the material-mix factor), normals from field gradient or face normals (caller choice), and SHALL offer an optional box-projection UV utility.

#### Scenario: Blend gradient in vertex colors
- **WHEN** two differently colored shapes joined by a smooth blend are meshed
- **THEN** vertex colors across the joint interpolate following the blend's material-mix falloff, not a hard color seam

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

### Requirement: Decimation reads an attribute only when it is aligned
`mesh::decimate` SHALL carry a normal, color or uv array through only when its length equals the position count, in every pass. Testing an attribute for emptiness instead admits a short array and indexes past its end — and a mesh imported from a file can carry one.

#### Scenario: A mesh with a short attribute array
- **WHEN** a mesh whose colors array is shorter than its positions array is decimated
- **THEN** decimation reads nothing out of bounds and drops the unaligned attribute

#### Scenario: An aligned mesh keeps its attributes
- **WHEN** a mesh whose attributes match its position count is decimated
- **THEN** the attributes are carried through as before

### Requirement: A mesher prices the grid its resolution implies
`mesh_tape` SHALL reject a voxel size that is not finite and positive, and a resolution whose implied dense lattice exceeds the module's documented sample ceiling, returning an empty mesh rather than sizing the allocation from the caller's number.

The ceiling SHALL admit the resolution the library's documentation advertises; a guard that turns documented usage into an error is a worse defect than the one it prevents.

#### Scenario: An over-fine voxel size yields an empty mesh
- **WHEN** `mesh_tape` is called with a voxel size so fine that the region needs more than the ceiling of lattice points
- **THEN** it returns an empty mesh and does not allocate the lattice

#### Scenario: A non-finite voxel size yields an empty mesh
- **WHEN** `mesh_tape` is called with a voxel size of zero, a negative, an infinity or a not-a-number
- **THEN** it returns an empty mesh

