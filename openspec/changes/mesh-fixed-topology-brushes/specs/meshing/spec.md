# meshing — a mesh gains neighbourhoods, and vertices that move

Delta for `mesh-fixed-topology-brushes`.

## ADDED Requirements

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

### Requirement: Surface-measured falloff
The library SHALL offer a falloff measured ALONG THE SURFACE — a bounded Dijkstra walk over the one-ring with Euclidean edge lengths, seeded at the class nearest the brush centre — in addition to a straight-line falloff.

The walk SHALL be deterministic: ties in the frontier SHALL break on class index, so the same brush on the same mesh reaches the same classes in the same order on every platform.

The straight-line falloff SHALL remain available and SHALL be the default for the verbs whose meaning is "everything under this disc".

#### Scenario: A lip does not drag the chin
- **WHEN** a surface-measured brush is placed on the upper lip of a closed-mouth head with a radius that reaches the chin through the closed mouth
- **THEN** the chin's vertices are outside the region and do not move

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
Each stamp SHALL be resolved against a PRE-STAMP SNAPSHOT of the region — positions, normals, the region's averaged normal, its centroid and its best-fit plane — so a composed verb is a single operation rather than a sequence of calls.

`scrape` SHALL be flatten-cut-only and smooth from ONE snapshot, and `crease` SHALL be a tight negative draw and a pinch summed within ONE stamp. Calling the components in sequence SHALL NOT be expected to produce the same result, for the reason `sculpt_scrape` already states.

`clay` SHALL clamp its deposit to a plane floating at the stamp height along the region's averaged normal, which is what makes flat-topped strips rather than a swell.

`polish` SHALL gate its smoothing by the agreement of the one-ring's normals: full strength where they agree within a threshold angle and falling to zero at twice it, so noise is removed and a hard edge survives.

`snakehook` SHALL re-anchor its falloff on the dragged position each stamp. Extreme triangle stretch under it SHALL be documented behaviour rather than a defect.

#### Scenario: Clay leaves a flat top
- **WHEN** a `clay` stroke is drawn across a sphere
- **THEN** the deposited strip's outer vertices lie on a common plane, and a `draw` stroke at the same settings does not

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
