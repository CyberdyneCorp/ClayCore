# c-abi — a host sculpts a mesh layer

Delta for `mesh-fixed-topology-brushes`.

## ADDED Requirements

### Requirement: A mesh sculpting session
The C ABI SHALL expose a **sculptor handle** created over a mesh, owning the two structures that are expensive to build and cheap to keep: the vertex adjacency and the ray-query BVH.

The handle SHALL be the entry point for stamping, stroking and picking, because building an adjacency per stamp is the whole cost of a stroke and an interface that hid the build would pay it every time.

The adjacency SHALL NOT need rebuilding after any verb, because no verb changes topology; the header SHALL say so. The BVH SHALL be refreshable by an explicit call, and the header SHALL state that positions moving without a refresh make picking report the surface as it was.

The handle SHALL keep the mesh alive for its lifetime and SHALL be destroyed by its own destroy call.

#### Scenario: A session is built once and stamped many times
- **WHEN** a host creates a sculptor over a mesh and applies a hundred stamps
- **THEN** the adjacency is built once, no call rebuilds it, and the mesh's index and quad buffers are unchanged throughout

### Requirement: The verbs across the ABI
The C ABI SHALL expose every verb — grab, draw, inflate, smooth, pinch, flatten, clay, crease, scrape, polish and snakehook — as enumerators on ONE versioned descriptor carrying the brush centre, radius, strength, falloff curve, direction, the surface-versus-straight-line falloff choice, the flatten mode with its optional explicit plane, the polish angle and the smoothing iteration count.

The descriptor SHALL carry the leading `uint32_t struct_size` every descriptor in this ABI carries.

An unknown verb, an unknown falloff and an unknown flatten mode SHALL each be REFUSED with `CLAY_ERROR_INVALID_ARGUMENT` rather than mapped onto the default, as the mesher enums already are. A non-positive radius SHALL be refused. A smoothing iteration count SHALL be bounded at this boundary, because each iteration walks the region again.

#### Scenario: An unknown verb is refused
- **WHEN** a host passes a verb value outside the declared list
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` with a detail message and the mesh is unchanged

#### Scenario: A cost knob nobody could have meant is refused
- **WHEN** a host passes a smoothing iteration count far above what any brush would spend
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the mesh is unchanged

### Requirement: Strokes and masks across the ABI
The C ABI SHALL expose a whole-stroke entry point taking the stroke preset descriptor and the stroke samples it already declares, resolving them through the same `resolve_stroke` the voxel and mask consumers use, and SHALL accept an optional mask handle gating every verb by `1 - mask`.

It SHALL report how many stamps moved a vertex.

#### Scenario: A stroke and a mask reach a mesh
- **WHEN** a host resolves a stroke with a preset and applies it to a sculptor with a mask covering half the region
- **THEN** only the unmasked vertices moved and the reported stamp count is the number that acted

### Requirement: Vertex deltas across the ABI
The C ABI SHALL expose a **delta record handle** that a stamp or a stroke writes into, reporting the number of vertices it holds, reverting the mesh to its pre-record state, re-applying, and clearing.

Reverting SHALL restore positions and normals bit-exactly. The record SHALL coalesce, so passing one record through a whole gesture yields one undo step.

#### Scenario: A host gets undo without snapshotting the mesh
- **WHEN** a host passes one record through a stroke of many stamps and then reverts it
- **THEN** the mesh's vertex buffer is byte-identical to its pre-stroke contents, and the record's vertex count is at most the number of vertices the stroke reached

### Requirement: Mesh picking across the ABI
The sculptor handle SHALL answer a ray query with a hit flag, the world-space position and normal, the triangle index, the barycentric coordinates and the ray parameter, taking the layer transform as the query's frame.

#### Scenario: A host turns a tap into a brush centre
- **WHEN** a host casts a ray at a mesh layer and feeds the returned position into a stamp descriptor
- **THEN** the stamp lands on the surface the ray hit
