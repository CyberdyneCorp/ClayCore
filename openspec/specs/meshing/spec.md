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

For a brick mesh, gradient normals and vertex colors SHALL be evaluated through per-brick culled tapes — each vertex against a tape culled to the band-dilated region of the brick owning its position, the same region the refill path culls against — so that the attribute cost follows the bricks being meshed rather than the total size of the document. Re-meshing a fixed brick set with gradient normals SHALL NOT scale with the number of document nodes outside those bricks' influence.

Culling SHALL NOT change the attributes: inside a brick's band-dilated cull region the culled tape's band-clamped results are bit-identical to the full tape's, mesh vertices lie on the surface far inside the band, and the gradient taps move by the gradient epsilon, which the cull region is additionally dilated by. The normals and colors SHALL equal a full-document-tape evaluation at every vertex.

#### Scenario: Blend gradient in vertex colors
- **WHEN** two differently colored shapes joined by a smooth blend are meshed
- **THEN** vertex colors across the joint interpolate following the blend's material-mix falloff, not a hard color seam

#### Scenario: Far nodes do not change a brick mesh's attributes
- **WHEN** a fixed brick set is meshed with gradient normals and colors from a document holding hundreds of nodes far outside those bricks' influence, and from a document holding only the nearby nodes
- **THEN** the two meshes carry identical geometry and equal normals and colors, and both equal the full document tape's gradient at every vertex

#### Scenario: Gradient normals cost the bricks, not the document
- **WHEN** the same fixed brick set is re-meshed with gradient normals as the document grows with far-away edits
- **THEN** the meshing time stays with the brick set rather than growing linearly with the document's node count, which the benchmark gate enforces as a ratio

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

### Requirement: The brick mesher marches a level
The brick mesher SHALL take the LEVEL to march, defaulting to the full-resolution one so that every existing call site is unaffected. A mip is the cache's own lattice with every second point kept, so a level SHALL change exactly two things — the lattice spacing doubles per level, and the keys are the coarse block keys — and SHALL change nothing about the marching, the seam welding, the per-key ranges or the straddler attribution.

Straddler collection SHALL test the neighbouring bricks AT THE SAME LEVEL, so a subset stays a filter of the whole mesh at that level rather than borrowing cells from another one.

A level the cache cannot hold SHALL produce an empty mesh rather than the nearest level it can.

Field attributes — colours and gradient normals — SHALL be applied at the full-resolution level only. They are evaluated through per-brick culled tapes whose exactness rests on a vertex sitting on the field's surface, and a coarse vertex sits on the mip's; the attributes would be silently approximate. Face normals are derived from the triangles and are unaffected.

#### Scenario: The coarse mesh is the same surface, coarser
- **WHEN** a filled cache whose mips are built is meshed at level 0 and at level 1
- **THEN** both meshes describe the same surface — bounds agreeing to within one coarse cell — and the coarse one carries substantially fewer triangles

#### Scenario: A level changes no other behaviour
- **WHEN** the whole-cache and key-subset paths are meshed at level 0 after the level parameter exists
- **THEN** the output is identical, vertex for vertex and index for index, to what the mesher produced before it existed

### Requirement: Vertex adjacency over weld classes
The library SHALL build, from a `mesh::Mesh`, a one-ring adjacency structure and a vertex-to-triangle map, in flat CSR arrays built in one pass.

Adjacency SHALL be built over **weld classes** rather than raw vertex indices: vertices whose positions coincide within a quantization epsilon form one class, and the one-ring is the graph over classes. The epsilon SHALL default to a fraction of the mesh's bounding-box diagonal so it does not change meaning with the model's authoring units, and an epsilon of zero SHALL mean exact-bit welding.

The structure SHALL expose the members of each class, and every operation built on it SHALL write one displacement to ALL members of a class, so coincident duplicates stay coincident and a UV seam cannot open into a crack.

The structure SHALL be checkable against a mesh (`matches`) by vertex and index count, and every entry point taking both SHALL check rather than trust.

#### Scenario: A seam does not split the ring
- **WHEN** adjacency is built over a mesh whose vertices are duplicated along a UV seam
- **THEN** the classes on either side of the seam are one class, and a walk over the ring crosses it

#### Scenario: Adjacency is rejected against the wrong mesh
- **WHEN** an adjacency built for one mesh is passed with a mesh of a different vertex or index count
- **THEN** the call is refused rather than reading out of bounds

### Requirement: A region measured along the surface
The library SHALL offer a region measured ALONG THE SURFACE in addition to a straight-line one. A class SHALL be in the surface-measured region when a path over the one-ring leads to it that (a) never leaves the ball of the brush radius and (b) is itself no longer than a path budget larger than that radius.

The walk SHALL be deterministic: ties in the frontier SHALL break on class index, so the same brush on the same mesh reaches the same classes in the same order on every platform.

The WEIGHT at a class SHALL come from the straight-line distance to the brush centre in both modes, never from the path length. An edge path overestimates geodesic distance by a direction-dependent amount, and a falloff driven by it bands visibly; bounding the region by the ball is what keeps the rim the falloff's own zero rather than wherever the walk happened to stop.

Consequently, on a CONNECTED sheet the two modes SHALL produce identical results: the surface measurement SHALL cost nothing where there is nothing for it to exclude.

The straight-line region SHALL remain available and SHALL be the default for the verbs whose meaning is "everything under this disc".

#### Scenario: A lip does not drag the chin
- **WHEN** a surface-measured brush is placed on the upper lip of a closed-mouth head with a radius that reaches the chin through the closed mouth
- **THEN** the chin's vertices are outside the region and do not move

#### Scenario: A walk over a structured grid is not clipped
- **WHEN** the same stamp is applied to a flat triangulated grid in both modes
- **THEN** the two results are bit-identical, and neither leaves a rim the falloff did not put there

### Requirement: Fixed-topology mesh brushes
The library SHALL provide vertex-displacement brushes over a mesh's own triangles: `grab`, `draw`, `inflate`, `smooth`, `pinch`, `flatten`, `clay`, `crease`, `scrape`, `polish` and `snakehook`.

**Topology SHALL NOT change.** No verb SHALL create, split, delete or reorder a polygon or a vertex. `Mesh::indices` and `Mesh::quads` SHALL be byte-identical before and after any verb, so a quad mesh sculpted here is still a quad mesh.

`draw` SHALL displace along the region's AVERAGED normal — one shared direction per stamp — and `inflate` SHALL displace along each vertex's OWN normal. That difference SHALL be the distinction between the two verbs.

`pinch` SHALL be ONE signed deformation, gathering tangentially toward the brush centre for a positive strength and spreading (magnify) for a negative one, matching the convention the field and voxel verbs already set. Its displacement SHALL be tangential — the component along the vertex normal SHALL be removed — so a pinch gathers along the surface rather than sinking the region.

`flatten` SHALL take the same `TwoSided` / `CutOnly` / `FillOnly` mode the field flatten established, because cut-only is Trim Dynamic and hPolish.

Every verb SHALL take an optional gate and SHALL scale each class's weight by `1 - gate`, so one rule masks all eleven verbs with no per-verb code.

Applying a verb SHALL be DETERMINISTIC: the same mesh, the same settings and the same stamps SHALL produce bit-identical positions on every run and every platform.

#### Scenario: Topology survives every verb
- **WHEN** each verb is applied to a quad-exported mesh
- **THEN** `indices` and `quads` are byte-identical to the input, and only `positions` and `normals` differ

#### Scenario: Draw and inflate differ where it matters
- **WHEN** `draw` and `inflate` are applied at equal strength to a region straddling a saddle
- **THEN** `draw` moves every vertex in one direction and `inflate` moves them along their own normals, and the two results differ

#### Scenario: A gate protects what it covers
- **WHEN** a displacement verb and `smooth` are each applied over a region half of which is fully gated
- **THEN** the gated vertices are bit-identical to their input positions and the ungated ones moved

### Requirement: One stamp is one operation
Each stamp SHALL be resolved against a PRE-STAMP SNAPSHOT of the region — positions, normals, the region's weighted average normal, its weighted centroid and the plane those two define (or the plane the caller gave) — so a composed verb is a single operation rather than a sequence of calls.

`scrape` SHALL be flatten-cut-only and smooth from ONE snapshot, and `crease` SHALL be a tight negative draw and a pinch summed within ONE stamp. Calling the components in sequence SHALL NOT be expected to produce the same result, for the reason `sculpt_scrape` already states.

`clay` SHALL clamp its deposit to a plane floating at the stamp height along the region's averaged normal — material is added UP TO the plane and no further — which is what makes flat-topped strips rather than a swell. On a flat surface it SHALL be indistinguishable from `draw`; the difference is what the two do to an uneven one.

`polish` SHALL gate its smoothing by the agreement of the one-ring's normals: full strength where they agree within a threshold angle and falling to zero at twice it, so noise is removed and a hard edge survives.

`snakehook` SHALL re-anchor its falloff on the dragged position each stamp. Extreme triangle stretch under it SHALL be documented behaviour rather than a defect.

#### Scenario: Clay leaves a flat top
- **WHEN** a `clay` stamp lands on an uneven surface
- **THEN** the deposit's outer vertices lie on a common plane, and a `draw` stamp at the same settings carries the unevenness up with it

#### Scenario: Polish keeps a hard edge
- **WHEN** `polish` runs over a noisy chamfered box across the chamfer
- **THEN** the noise on the flats is reduced and the dihedral angle at the chamfer is preserved within a tolerance a plain `smooth` at the same settings does not meet

### Requirement: Recomputed normals for the touched region
After a verb runs, the library SHALL recompute vertex normals for the vertices it moved AND their one-ring, because a triangle's normal changes when any corner moves. Recomputation SHALL be area-weighted.

Recomputation SHALL be per stamp by default and SHALL be deferrable to the end of a stroke at the caller's choice.

A mesh carrying NO normals SHALL still carry none afterwards: normals are optional on `mesh::Mesh` and manufacturing them would change what the layer exports.

#### Scenario: Normals follow the vertices
- **WHEN** a `draw` stamp raises a dome on a plane
- **THEN** the normals inside the dome point outward from it, and the normals outside the touched region and its ring are unchanged

### Requirement: Sparse vertex deltas restore a mesh exactly
The library SHALL record, per gesture, the vertices a stroke actually reached with their positions and normals before and after, and SHALL restore the mesh from that record.

The record SHALL be COALESCED: a vertex touched by many stamps of one stroke SHALL appear once, keeping the first `before` and the last `after`, so its size is bounded by the vertices reached rather than by the stamps taken.

Reverting SHALL restore the mesh BIT-EXACTLY, including normals, which SHALL be stored rather than recomputed — an imported mesh's normals are its author's and a recomputed replacement is a different mesh.

Reverting and re-applying SHALL each be idempotent, and neither SHALL touch `indices` or `quads`.

#### Scenario: Undo is bit-exact
- **WHEN** a multi-stamp stroke runs over an imported mesh and its deltas are reverted
- **THEN** every position and normal byte equals the pre-stroke mesh, and `indices` and `quads` are unchanged

#### Scenario: One gesture is one record
- **WHEN** a stroke of forty stamps passes repeatedly over the same twenty vertices
- **THEN** the record holds twenty entries, not eight hundred

### Requirement: The BVH names the triangle it hit
`mesh::Bvh` SHALL retain, for each triangle it partitioned, that triangle's index in the source mesh, and SHALL answer a ray query with the nearest hit's distance, source triangle index and barycentric coordinates.

Distance and winding-number queries SHALL be unchanged by this addition.

Ray queries SHALL NOT cull back faces: a sculptor working on the inside of a shell means it.

#### Scenario: A ray names a triangle
- **WHEN** a ray is cast at a mesh through two of its triangles
- **THEN** the nearer triangle's source index is returned, with barycentrics that reconstruct the hit position from that triangle's corners

#### Scenario: The BVH still measures the same distances
- **WHEN** the same distance and winding queries are run before and after this change
- **THEN** the results are identical

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

The dual is chosen over greedy merging because merged rectangles create T-JUNCTIONS where one long quad abuts several short ones, and a T-junction cracks under subdivision and splits normals. No vertex of the dual lands in the interior of another face's edge. Over a CONTINUOUS field — an SDF tape — every quad edge is then shared by at most two quads. That bound SHALL NOT be stated unqualified, because the same mesher over a voxel occupancy field does exceed it: two solid cells meeting only along a lattice EDGE put that quad edge in FOUR quads, and a checkerboard is nothing but that case. It is the shape having no manifold surface to find, not the mesher erring, and it is why the non-manifold statement below has two causes and not one.

Vertex valence AVERAGES exactly four — every quad contributes four corners and the mesh has as many vertices as quads — but SHALL NOT be claimed to be four everywhere. A cell the surface enters through a corner has six of its twelve lattice edges change sign and belongs to six quads; one clipped by a corner belongs to three. Measured on a sphere the distribution is roughly 55% valence four with the rest at three, five and six, and it does not tighten with resolution: it is the lattice's discrete curvature, and a mesher that placed valence four everywhere would be doing retopology.

The mesher SHALL NOT change the diagonal on which it triangulates. It triangulates on the 0–2 diagonal today; a non-planar quad would triangulate better on its shorter one, and switching would change the triangles every existing caller of the dual meshers receives.

Two properties of the output SHALL be stated in the header rather than discovered:

- **The quads are NOT planar.** Four cell vertices around a lattice edge are placed independently and nothing makes them coplanar, so a consumer that triangulates on its own terms may shade a face differently than this library's triangulation does. No planarisation is performed: flattening a face moves vertices off the surface, trading a shading artifact for a geometric error.
- **The output is NOT manifold and NOT watertight**, for TWO reasons, only the first of which the surface-nets header already gives: a cell the surface crosses twice gets a single vertex and pinches the sheets; and DIAGONAL occupancy, which a voxel sculpt produces constantly, puts one quad edge in FOUR quads where two cells meet along a lattice edge, and shares a single vertex between two disjoint sheets — a bowtie — where they meet only at a corner. The marching mesher remains the watertight, 2-manifold export path and is unaffected.

#### Scenario: A quad-meshed sphere is quads all the way
- **WHEN** a sphere tape is quad-meshed
- **THEN** every face in the output is a quad, no vertex lies in the interior of another face's edge, no quad edge is shared by more than two quads, and vertex valence stays between three and six with an average of four

#### Scenario: The quad mesh is the dual mesh
- **WHEN** a tape is meshed with the dual mesher and quad-meshed at the same cell size
- **THEN** the positions and the triangle indices are identical, and only the quad array differs

### Requirement: A quad count is a target the mesher approaches, not one it hits
The module SHALL accept a target quad count and search for the lattice cell size that comes closest to it, reporting the cell size it settled on, the count it actually produced, how many iterations it took, and whether it landed inside the requested tolerance.

**The target is a HINT with a reported actual — not a ceiling and not an exact count.** A ceiling SHALL NOT be promised, because quad count is not monotonic in cell size: a finer lattice can resolve a thin feature the coarser one missed entirely and so ADD area and quads. Where two candidates are equally close, the search SHALL prefer the one that does not exceed the target.

Because count scales as the inverse square of cell size, a one percent change in cell size moves the count about two percent. The documented expectation SHALL be a landing inside roughly five to ten percent of a target within the default iteration cap; the default tolerance SHALL be ten percent, and a tolerance far below that SHALL be expected to exhaust the cap and return the best attempt with the tolerance flag clear, rather than iterating without bound.

Each iteration is a full mesh, including a full dense field evaluation on the tape path. The iteration cap SHALL therefore be a caller-visible cost knob with a small default, and the header SHALL say so.

The search SHALL clamp rather than fail:

- It SHALL never request a lattice the mesher's own resolution pricing would refuse; it stops at that ceiling and reports that it clamped. A caller MAY impose a floor of its own on top of that one, and reaching either is reported the same way. The ceiling SHALL be a cell size that pricing accepts IN THE TYPE THE MESHER TAKES, not merely one near the bound: a floor arrived at by rounding a wider computation is on the wrong side of the bound half the time, and a refused floor turns a resolution limit into an empty mesh.
- It SHALL also bound the COARSE end at a lattice too sparse to span the region at all, and report reaching that as clamped for the same reason: the search stopped at a limit rather than at the target.
- When the shape's own topology collapses before the target is reached, it SHALL return its best result and report that it did not converge, rather than returning the collapse as the answer. A candidate that produced NO quads SHALL never be preferred over one that produced some.

The clamped flag and the tolerance flag are independent. A search that stopped at a limit and landed inside the tolerance there SHALL report both, because both happened.

The search SHALL be one implementation, shared by every source that has a cell size, so the tape path and the voxel path cannot drift into two different contracts for the same promise.

Decimation SHALL NOT be used to approach a target: quadric edge collapse is a triangle operation and breaks the quad pairing on its first collapse.

#### Scenario: A target is approached and the actual is reported
- **WHEN** a shape is quad-meshed with a target of twenty thousand quads at the default tolerance
- **THEN** the returned mesh's quad count is within ten percent of twenty thousand, and the report states that count, the cell size used and that it converged

#### Scenario: An unreachable target reports rather than lies
- **WHEN** a target is requested that the resolution ceiling cannot reach
- **THEN** the mesher returns the mesh at the finest lattice it is allowed, and the report states the actual count, the cell size, and that the search clamped

#### Scenario: The limit the search clamps to is one the mesher accepts
- **WHEN** the fine limit of the search is computed for a region
- **THEN** the mesher's own pricing accepts that cell size, and one representable step finer is refused — so a target beyond the ceiling returns that lattice's mesh and never an empty one reported as a resolution limit

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

### Requirement: The brick mesher accepts an explicit brick set
The brick mesher SHALL accept an explicit set of brick keys to march as well as the cache's whole surface set. Marching a subset SHALL sample across the subset's boundary exactly as the whole-surface mesh does, so that a cell's crossing is resolved from the field rather than from what happens to be in the set.

Consequently the triangles a subset mesh produces for a cell SHALL be identical to those the whole-surface mesh produces for that cell. A subset SHALL differ from the corresponding part of the whole only in vertex sharing: a vertex on the boundary of the subset is welded within the subset and emitted again by any other mesh that reaches it, at an identical position. A subset SHALL NOT introduce a crack, a hole, or a displaced boundary vertex.

A subset mesh SHALL contain every triangle of the whole-surface mesh with at least one corner inside a requested brick's closed box — including the straddlers, whose cell is owned by an unrequested brick — and SHALL contain no triangle the whole-surface mesh does not. Each straddler SHALL be attributed to exactly one requested key, chosen deterministically as the lexicographically lowest (x, then y, then z) requested key whose closed box contains one of the triangle's corners, so that a consumer holding geometry per brick can dedupe. Straddler vertices SHALL weld onto the subset's own vertices exactly as the whole mesh welds them, at bit-identical positions.

The mesher SHALL be able to report, for each key in the order given, the contiguous range of vertices and of indices that key contributed — the key's own cells' triangles first, then its attributed straddlers. The ranges SHALL partition the output, and the header SHALL state that welding spans brick seams — so a triangle in one key's index range may reference a vertex in an earlier key's vertex range, and a consumer may overwrite a key's ranges but may not free them in isolation.

Whole-surface meshing SHALL remain the default and SHALL remain watertight and 2-manifold, since it is also the export path. It marches every surface cell already, so it has no boundary and owes no straddlers; naming every surface brick as the subset SHALL produce exactly the whole-surface mesh.

#### Scenario: A subset agrees with the whole
- **WHEN** a cache is meshed whole, and then a subset of its keys is meshed alone
- **THEN** every triangle in the subset mesh appears in the whole mesh at the same world positions, and no cell owned by those keys is missing from either

#### Scenario: A straddler is emitted, not omitted
- **WHEN** an edit's dirty keys are meshed as a subset and the whole surface is meshed from the same cache state
- **THEN** the triangles with at least one corner inside the requested bricks are the same set in both meshes — none missing from the subset, none invented by it

#### Scenario: A per-brick consumer can maintain a surface
- **WHEN** a consumer replaces each dirty key's stored share from the subset's per-key ranges after every edit, deduplicating repeated triangles
- **THEN** the union of the stored shares equals a whole-surface rebuild of the same document

#### Scenario: A boundary vertex is duplicated, not moved
- **WHEN** two adjacent bricks are meshed as two separate subsets
- **THEN** the vertices on their shared seam appear in both meshes at bit-identical positions, leaving no gap when the two are drawn together

#### Scenario: Export is unaffected
- **WHEN** the whole surface is meshed with no key set named
- **THEN** the result is the watertight, 2-manifold mesh the default mesher already guarantees

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

### Requirement: Voxel meshing names a level
Greedy meshing of a voxel grid SHALL take the level it meshes explicitly, and the form that does not name one SHALL mesh the ACTIVE level. A level a grid does not have SHALL produce an empty mesh rather than a guess.

Meshing is not defined as "always the finest": a block-out pass wants the coarse form at interactive cost, and picking the finest silently would make the level stack unusable for the workflow it exists for.

#### Scenario: The same solid meshes to the same extent at every level
- **WHEN** a grid that has only been subdivided is meshed at two different levels
- **THEN** both meshes span the same world-space extent, because the cell size halved with the cell count

#### Scenario: A one-level grid is unchanged
- **WHEN** a grid with a single level is meshed without naming a level
- **THEN** the mesh is identical to the one the same grid produced before levels existed

