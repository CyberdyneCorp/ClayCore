# meshing — global voxel remeshing

Delta for `add-voxel-remesher`.

## ADDED Requirements

### Requirement: Global voxel remesh
The library SHALL provide a global voxel-remesh operation that takes a polygonal surface, samples it into a signed volumetric representation at an explicit spatial resolution, and extracts a new polygonal surface from that representation.

The operation SHALL be a single semantic verb reachable by a consumer of the library, not an assembly a caller performs out of the sampling, meshing and validation primitives. It SHALL compose those existing primitives — the mesh BVH, its generalized winding sign, the sparse narrow-band sampled field, the default watertight mesher, the validator and the attribute transfer — and SHALL NOT introduce a second implementation of any of them.

Overlapping shells and self-intersections SHALL be resolved volumetrically: where two surfaces of one input overlap, the result SHALL describe the occupied region with one exterior surface rather than reproducing both shells.

The result SHALL NOT preserve source vertex or polygon identity. Vertex count, triangle count, ordering and connectivity are all replaced, and no consumer may index the result with a source index.

#### Scenario: Overlapping shells fuse
- **WHEN** two spheres whose interiors overlap are remeshed as one input
- **THEN** the result is a single connected watertight component enclosing their union, not two intersecting shells

#### Scenario: Stretched topology is rebuilt
- **WHEN** a surface whose triangles have been stretched far past uniformity is remeshed
- **THEN** the result's edge lengths cluster around the requested spatial resolution rather than reproducing the input's distribution

#### Scenario: Identity is not preserved
- **WHEN** a mesh is remeshed
- **THEN** the report states the source and result vertex and triangle counts separately, and nothing in the API maps a source index onto a result index

### Requirement: Voxel remesh resolution is a physical size
The canonical resolution of a voxel remesh SHALL be a voxel size in world units. A longest-axis resolution MAY be offered as a convenience, and SHALL map onto the canonical size as the source's longest bounding extent divided by that resolution, resolved BEFORE any sampling padding is applied.

The resolved voxel size SHALL be reported by both the estimate and the result, because it — not the resolution integer — is what predicts which features survive.

A voxel size that is not finite and positive, or a longest-axis resolution of zero, SHALL fail with a typed status and produce no mesh.

#### Scenario: Two spellings of one resolution agree
- **WHEN** a mesh is remeshed at longest-axis resolution N, and again at the voxel size that resolution resolved to
- **THEN** both runs report the same resolved voxel size and produce the identical mesh

#### Scenario: Finer resolution keeps more of the source
- **WHEN** a model is remeshed at two voxel sizes, one half the other
- **THEN** the finer result's symmetric surface distance to the source is smaller than the coarser result's

#### Scenario: An invalid resolution is refused
- **WHEN** a remesh is requested with a zero, negative, infinite or NaN voxel size
- **THEN** it fails with an invalid-resolution status and returns no mesh

### Requirement: Voxel remesh preflights its cost
The library SHALL provide an estimate of a voxel remesh that runs before the operation and without performing it, reporting at least: the resolved voxel size, the grid dimensions, the number of active samples the sparse domain will hold, the estimated working memory in bytes, a range for the result triangle count, the source's open-boundary edge count and connected-component count, and whether the source carries features at risk of being lost at this resolution.

A request whose estimated memory exceeds a caller-supplied budget, or which exceeds the library's own ceilings on active samples or lattice cells, SHALL fail with a typed resource status BEFORE allocating the field, the acceleration structure or the result.

The library SHALL NOT silently reduce a requested resolution to fit a budget. Fitting a resolution to a budget is a caller policy built from repeated estimates.

#### Scenario: An oversized request is refused before it allocates
- **WHEN** a remesh is requested at a resolution whose estimated memory exceeds the supplied budget
- **THEN** it returns an exceeds-budget status, the estimate that justified it, and no mesh, without having allocated the field
- **AND** the peak memory of the refused call is a small fraction of the estimate it refused

#### Scenario: The estimate predicts the run
- **WHEN** a remesh is estimated and then performed at the same parameters
- **THEN** the reported resolved voxel size matches, the result triangle count falls inside the estimated range, and the actual active sample count does not exceed the estimate

#### Scenario: A resolution is never quietly lowered
- **WHEN** a remesh at a resolution over budget is requested
- **THEN** no mesh is produced at any other resolution

### Requirement: Voxel remesh sampling follows the surface
The sampling domain of a voxel remesh SHALL be marked from the source geometry and its narrow band, so that the number of expensive per-sample evaluations follows the source's surface area and the band's thickness rather than the volume of the source's bounding box.

The samples the sparse domain STORES SHALL be bit-identical to those a dense evaluation of the same region at the same voxel size, band and sign parameters would store, for a source with no open boundaries. Sparsity is an optimisation of the same field, not a different field.

For a source WITH open boundaries the two MAY differ, and only in the recorded sign of bricks that store no samples: the generalized winding number's half-crossing can fall away from every triangle, where the dense path resolves it per brick and the sparse path resolves it per connected region. This SHALL be stated in the API rather than left to be discovered.

Large remesh requests SHALL use sparse narrow-band storage. A dense lattice over the bounding box SHALL NOT be the normal production path at any resolution the API accepts.

#### Scenario: Sparse and dense agree on a closed source
- **WHEN** a closed mesh is remeshed and the same region is sampled densely at the same voxel size and band
- **THEN** the two fields store the same bricks and every stored sample is bit-identical

#### Scenario: A long thin model does not pay for its bounding box
- **WHEN** a long thin model is remeshed at a fixed voxel size, and again after being made longer along one axis at the same spacing
- **THEN** the active sample count and the sampling time grow with the added surface, not with the cube of the bounding box's growth
- **AND** the field's stored sample count stays a small fraction of the bounding box's lattice point count

### Requirement: Voxel remesh open-surface policy
A voxel remesh SHALL take an explicit policy for a source with open boundaries: reject it, close it, or proceed best-effort.

`Reject` SHALL return a typed status and no mesh. `Close` SHALL produce the closed volumetric interpretation and SHALL return a typed failure rather than an invalid mesh if the result is not watertight and 2-manifold. `BestEffort` SHALL proceed and SHALL report that the source was open and whether the result met the normal guarantees.

An open source SHALL NEVER be silently treated as though it had been watertight. The report SHALL carry the source's boundary-edge count whatever the policy.

#### Scenario: Reject leaves the caller with a reason
- **WHEN** a mesh with a hole is remeshed under `Reject`
- **THEN** the call fails with an open-surface status and the report names the source's boundary-edge count

#### Scenario: Close produces a watertight body
- **WHEN** a sphere with triangles removed is remeshed under `Close` in the default surface mode
- **THEN** the result is watertight, 2-manifold and consistently oriented, and the report records that the source was open

#### Scenario: Best effort says what it did
- **WHEN** an open surface is remeshed under `BestEffort`
- **THEN** the result is returned with the source recorded as open and the result's own boundary-edge count reported

### Requirement: Voxel remesh output is validated
The default surface mode of a voxel remesh SHALL produce a watertight, 2-manifold, consistently oriented mesh with no degenerate triangles and no non-finite vertex positions, and SHALL validate that before returning rather than asserting it.

A result failing that contract SHALL be reported as a typed failure and SHALL NOT be returned as a successful remesh, except under `BestEffort`, which returns it with the failure recorded.

A sharp surface mode MAY be offered and SHALL be marked experimental. The watertight contract above SHALL NOT be claimed for it, matching the existing flagged status of dual contouring.

The result SHALL carry a report holding at least: the resolved voxel size, source and result vertex and triangle counts, source and result signed volume and their relative difference, source and result boundary-edge and connected-component counts, whether the source was open, whether projection ran, and which attribute channels moved.

#### Scenario: The default mode's output is clean
- **WHEN** each fixture in the golden set is remeshed in the default surface mode
- **THEN** every result validates as watertight, 2-manifold and oriented, with zero degenerate triangles and every position finite

#### Scenario: A report accompanies every success
- **WHEN** a remesh succeeds
- **THEN** the report carries both meshes' counts and volumes, the relative volume difference, and the resolved voxel size

### Requirement: Voxel remesh preserves attributes spatially
Where a voxel remesh is asked to preserve vertex colour, it SHALL resample it from the source by closest point through the existing attribute transfer, never by source vertex index. A source carrying no colour SHALL produce a result carrying no colour: the operation SHALL NOT invent one.

The library SHALL provide a spatial resampling of a caller-owned per-vertex scalar array — a mask, a weight — from one mesh onto another, so that a mask held outside `mesh::Mesh` survives a remesh without the mask's storage being moved into the mesh to suit this operation.

UVs SHALL be dropped by the operation, and the API SHALL say dropped. A spatially reprojected UV is not a preserved UV layout and SHALL NOT be presented as one.

A malformed source attribute array — one whose length is neither zero nor the vertex count — SHALL be treated as absent rather than read.

#### Scenario: Colour survives a rebuild
- **WHEN** a mesh whose vertex colours vary smoothly across it is remeshed with colour preservation on
- **THEN** every result vertex carries approximately the source colour at its own position, and the report records that colour moved

#### Scenario: Absent attributes stay absent
- **WHEN** a mesh carrying no colour and no UVs is remeshed with preservation requested
- **THEN** the result carries no colour and no UVs, and the report records that neither moved

#### Scenario: A mask is resampled onto new topology
- **WHEN** a per-vertex mask over a source mesh is transferred onto a remeshed result
- **THEN** the masked region of the result covers the same volume of space the source's masked region did, within the voxel size

### Requirement: Voxel remesh projection is clamped
Where a voxel remesh is asked to project its result back onto the source, each result vertex SHALL move toward the closest point on the source by at most a configured fraction of the way, and SHALL NOT move at all when the closest point is further than a configured multiple of the voxel size, or when the source surface there faces away from the vertex's own reconstructed normal.

Projection SHALL NOT hard-snap vertices onto the source. A rejected candidate SHALL leave its vertex where extraction placed it.

#### Scenario: Projection recovers detail
- **WHEN** a model is remeshed with projection on and off at the same resolution
- **THEN** the projected result's symmetric surface distance to the source is smaller

#### Scenario: Projection does not cross a gap
- **WHEN** a shape with two surfaces closer together than the projection clamp is remeshed with projection on
- **THEN** no result vertex is moved onto the surface facing away from it, and the result stays free of self-intersection introduced by projection

#### Scenario: The clamp is honoured
- **WHEN** a remesh projects with a maximum distance of k voxels
- **THEN** no vertex moves further than the projection strength times k voxel sizes

### Requirement: Voxel remesh component and volume policy
A voxel remesh SHALL preserve disconnected components by default. Removing components below a volume threshold SHALL be an explicit opt-in with an explicit threshold, because a floating component is as likely to be a tooth, a lash or an armour plate as it is to be debris.

Where volume preservation is requested, the operation MAY apply a uniform correction about the result's centroid, bounded by a strict clamp, and SHALL skip it where the topology changed in a way that makes the comparison meaningless — an open source closed under policy, or components removed. The report SHALL carry both volumes and their relative difference whether or not a correction was applied.

#### Scenario: A floating component is kept by default
- **WHEN** a mesh with a large body and a small separate island is remeshed with the default policy
- **THEN** the result has two components

#### Scenario: Removal is by the caller's threshold
- **WHEN** the same mesh is remeshed with removal below a volume that the island falls under
- **THEN** the result has one component and the report records the component counts before and after

### Requirement: Voxel remesh is cancellable and reports progress
A voxel remesh SHALL be cancellable through the library's cooperative cancellation token, and SHALL check for cancellation at bounded intervals within every expensive stage — domain marking, field sampling, extraction, projection and attribute transfer — rather than only between stages.

A cancelled remesh SHALL return a cancelled status and no result mesh, and SHALL leave the source mesh unmodified.

The operation SHALL publish its progress through the same token, naming the stage it is in.

#### Scenario: Cancellation during sampling is non-destructive
- **WHEN** a remesh is cancelled while sampling the field
- **THEN** it returns a cancelled status with no mesh, and the source mesh is byte-identical to what it was before the call

#### Scenario: Cancellation does not wait for a stage to end
- **WHEN** a token is cancelled during a multi-second stage
- **THEN** the call returns without completing that stage

### Requirement: Voxel remesh is deterministic
The CPU reference path of a voxel remesh SHALL be deterministic: the same source mesh and the same parameters SHALL produce a bit-identical result mesh — positions, indices and every attribute — on every run of the same build, whatever the thread pool's scheduling.

This SHALL be a property of how the work is decomposed rather than of running it on one thread: every parallel stage SHALL write disjoint outputs computed from position-only inputs.

#### Scenario: Repeated runs are bit-identical
- **WHEN** the same mesh is remeshed twice at the same parameters in the same process
- **THEN** the two results are byte-identical in positions, indices, normals and colours

### Requirement: Parallel lattice marching is a public mesher
The library SHALL expose a parallel form of the lattice marcher, for a sample function that is safe to call concurrently, producing a mesh byte-identical to the serial marcher's.

Byte-identity SHALL be structural: parallel slabs record their crossings without welding and one builder replays them in slab order, so the builder observes the same call sequence the serial march makes.

#### Scenario: Parallel and serial marches agree exactly
- **WHEN** a lattice is marched serially and in parallel from the same pure sample function
- **THEN** the two meshes are byte-identical in positions and indices
