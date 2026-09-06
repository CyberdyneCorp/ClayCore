# meshing — one runtime around the shared kernels

Delta for `add-shared-brush-runtime`.

## ADDED Requirements

### Requirement: Work under a brush is addressed by a representation-neutral identity
The brush workset SHALL address the work one stamp touches by a neutral 64-bit work-item identity rather than by any one representation's vertex numbering, and every sculptor SHALL fill the SAME workset type.

The width SHALL be 64 bits because that is what the representations already need: an adaptive surface's vertex handle carries a slot AND a generation, and a hierarchy addresses a vertex as a level and an index. A narrower identity cannot carry either without losing the part that makes a stale handle detectable.

Each representation SHALL supply an adapter that walks its own surface and fills the neutral workset — the fixed mesh's weld-class walk, the adaptive surface's half-edge walk, the hierarchy's delegation to its bound level. The WALK is representation-specific and SHALL stay so. The COMPOSITION that follows it — the weight's factors in their one fixed order, the drop of a zero-weight entry, the automask, the resolution of the stamp's average normal, centroid and plane — SHALL exist once and name no representation.

A sculptor SHALL NOT keep a private set of parallel arrays that duplicates the workset. Everything composed into the weight reaches every sculptor by construction when there is one workset, and reaches only the sculptors somebody remembered when there are three.

#### Scenario: Three representations, one workset type
- **WHEN** the fixed, adaptive and multiresolution sculptors each gather a stamp
- **THEN** each has filled the shared workset type, and the composition step that produced their weights is the same function

#### Scenario: The neutral composition names no representation
- **WHEN** the composition step is compiled against a translation unit that includes no mesh, adjacency, adaptive-surface or hierarchy header
- **THEN** it compiles

### Requirement: A stamp's transient storage comes from a per-sculptor arena
Each sculptor SHALL own a scratch arena serving the storage one stamp needs and discards: the affected-item list, temporary normals, the surface traversal's frontier, alpha samples, automask weights, topology candidates and dirty patch lists. The arena SHALL allocate by bumping a pointer, SHALL reset by returning that pointer without freeing, and SHALL keep a capacity that tracks the largest recent footprint.

The arena SHALL be a member of the sculptor and SHALL NOT be a process-global mutable object. Several sculptors are live at once — a multiresolution sculptor OWNS a fixed one, and a document holds several mesh layers — and a shared arena would make one stamp's scratch depend on what another was doing, and would be a data race the first time a host stamped two layers on two threads.

The arena SHALL refuse, at compile time, any type whose destructor must run. Reset is a pointer store rather than a walk, so a type that owns memory would leak once per stamp at pointer rates.

After warm-up, an ordinary stamp on a stable-topology surface SHALL perform no heap allocation ON ANY REPRESENTATION and WITH AUTOMASKING ENABLED. Growth on first encountering a larger footprint is permitted; steady repeated local sculpting is not. The arena SHALL also report its high-water mark and how many times it has grown, so that scratch which grows every stamp and is never reset — which allocates nothing after warm-up and consumes memory without bound — is visible as a failure rather than as a pass.

#### Scenario: A warm automasked stamp allocates nothing
- **WHEN** a stroke of many stamps of similar footprint runs with the boundary, connectivity and normal-angle automask factors enabled, after the first stamp has warmed the arena
- **THEN** the instrumented allocation count for the subsequent stamps is zero

#### Scenario: A warm adaptive stamp allocates nothing
- **WHEN** the same stroke runs on an adaptive surface with topology changes disabled, so the surface is stable
- **THEN** the instrumented allocation count for the subsequent stamps is zero

#### Scenario: The arena converges
- **WHEN** a stroke of many stamps of similar footprint runs
- **THEN** the arena's growth count stops increasing, and its high-water mark stops rising

### Requirement: A brush stamp has an oriented frame on the surface
The library SHALL build, once per stamp, an orthonormal frame on the surface — an origin, a normal, a tangent, a bitangent and the rotation that oriented them — from the stamp's placement, the surface normal under it, and an azimuth or an explicit rotation the caller supplied.

That frame SHALL be what a stamp's (u, v) is measured in, and the SAME frame SHALL serve the alpha, the rake, the chisel, the clay strips and any other directional verb, on every representation. A directional brush that needs a frame of its own is evidence the shared frame is missing something, and the shared frame is what SHALL be extended.

A zero azimuth SHALL produce the unrotated basis without evaluating the rotation. This is a correctness rule and not an optimisation: multiplying by an exact 1 and adding an exact 0 is not the identity for a negative-zero component, and the resulting sign flip is visible in a bilinear alpha sample at a texel boundary.

An explicit rotation SHALL replace the azimuth rather than composing with it, so that a caller who supplies one gets exactly what they supplied.

#### Scenario: A zero azimuth changes nothing
- **WHEN** a stamp frame is built with no azimuth and again with an azimuth of zero
- **THEN** the two frames are byte-identical

#### Scenario: One frame, three representations
- **WHEN** the same stamp with the same alpha and the same azimuth is applied to a fixed mesh, an adaptive surface and a hierarchy at the same place with the same surface normal
- **THEN** the three frames are byte-identical

### Requirement: Brush semantics agree across representations where they are mathematically required to
Where two representations hold the same vertices at the same positions, a stamp that reads no surface-derived estimator SHALL produce byte-identical results on both. "Reads no estimator" is the precise condition: a verb given an explicit direction, a Euclidean footprint and no normal-dependent factor depends only on the shared weight composition and the shared kernel, and any difference between two representations is therefore a difference in the runtime around them.

Where two representations legitimately differ, the difference SHALL be NAMED and SHALL be asserted as a difference rather than left unstated. The vertex-normal estimators differ by construction — the fixed mesh's is angle-weighted and reaches `acos`, the adaptive surface averages the face normals it already caches — so every verb that reads a normal differs, and a gate that only checked for agreement would be satisfied by two representations that had become equally wrong.

A factor that is topological rather than geometric — a connectivity flood, a boundary-ring spread — SHALL agree EXACTLY across representations holding the same topology, because such a factor is set-valued and no float ordering can reach it.

#### Scenario: A normal-free stamp is byte-identical across representations
- **WHEN** a grab with an explicit direction, a Euclidean footprint and no automask is applied to a fixed mesh and to an adaptive surface built from that same mesh with topology changes disabled
- **THEN** the resulting positions are byte-identical

#### Scenario: A named difference still differs
- **WHEN** a draw is applied to the same two representations
- **THEN** the results differ, by no more than the stated bound, and the test asserts the difference rather than an equality

#### Scenario: A topological automask agrees exactly
- **WHEN** the boundary and connectivity automask factors are evaluated over the same topology on two representations
- **THEN** the resulting per-vertex factors are equal
