# dynamic-topology Specification

## Purpose
Sculpting where the triangles follow the brush instead of constraining it.

A mutable surface with identities stable across edits, local split, collapse and
flip that are atomic and refuse to corrupt, and a remesher driven by brush-
relative detail rather than by a global target. What it promises beyond "it
changes topology" is that the change is BOUNDED: a dab costs what it touches, a
gesture is one sparse undo step, the result is deterministic, and a host is told
which chunks moved instead of being handed the whole surface.

Its own capability rather than part of meshing because meshing turns a field
into triangles once, and this owns triangles that keep changing.

## Requirements

### Requirement: A mutable surface with stable identities
The library SHALL provide a triangular surface representation whose vertices, edges, half-edges and faces are addressed by handles that survive unrelated local edits, and SHALL NOT renumber unaffected elements when connectivity changes.

Handles SHALL carry a generation, so that a handle to a deleted element whose slot was reused is detected rather than silently referring to a different element.

The representation SHALL be separate from `mesh::Mesh`, which remains the flat interchange format. A dynamic surface SHALL import from and export to `mesh::Mesh` at explicit boundaries and SHALL NOT change what any existing mesh producer or consumer sees.

Attribute domains SHALL be distinguished: UVs are corner data and colour and mask are vertex data. A representation that cannot express one geometric vertex carrying two UVs cannot preserve a seam under a split, and retrofitting the distinction would mean rewriting every operator that interpolates.

A dynamic surface SHALL be triangles. On export `quads` SHALL be empty, and the documentation SHALL state that a quad workflow does not pass through this representation.

#### Scenario: A local edit renumbers nothing else
- **WHEN** an edge in one region is split and collapsed
- **THEN** handles to elements outside the affected neighbourhood still name the same elements

#### Scenario: A stale handle is detected
- **WHEN** an element is deleted, its slot reused by a later element, and the original handle is dereferenced
- **THEN** the lookup fails rather than returning the new element

#### Scenario: A round trip preserves the surface
- **WHEN** a mesh is imported into a dynamic surface and exported again with no edits
- **THEN** the geometry and the supported attributes are preserved under the stated seam-duplication semantics, and `quads` is empty

### Requirement: Local topology operators are atomic and refuse corruption
The library SHALL provide edge split, edge collapse and edge flip over the mutable surface.

Each operator SHALL be ATOMIC: it either applies completely or leaves the surface exactly as it found it. A refused, failed or cancelled operation SHALL NOT leave a partially applied edit behind.

A collapse SHALL be refused when it would invert a triangle, duplicate a triangle, produce a non-manifold edge, corrupt a boundary, destroy a UV seam, flip a local normal past a threshold, or produce a zero-area face. Validity SHALL be decided by a topological link-condition test and not by geometry alone.

A flip SHALL be refused on a boundary, on a constrained edge, where the resulting diagonal already exists, where face orientation would invert, and where the quality metric does not improve.

A split SHALL interpolate position, colour, mask and corner UVs, and normals SHALL be recomputed locally rather than treated as interpolated truth.

Edges SHALL carry constraints — boundary, UV seam, sharp, material and user-locked — and an operator SHALL honour them itself rather than relying on its caller to have filtered the input.

#### Scenario: A collapse that would invert is refused
- **WHEN** a collapse is requested whose result would invert an adjacent triangle
- **THEN** the operation reports no change and every invariant still holds

#### Scenario: A seam survives a split
- **WHEN** an edge on a UV seam is split
- **THEN** the seam is still present, both corner UVs are interpolated on their own side, and the surface does not crack

#### Scenario: Every invariant survives a fuzz run
- **WHEN** thousands of randomized split, collapse, flip and move operations run over randomized valid patches
- **THEN** the validator reports every invariant intact after every operation

### Requirement: Remeshing is local, bounded and driven by brush detail
The library SHALL adapt local edge lengths toward a target under a brush: splitting edges longer than the target by a factor, collapsing edges shorter than it by a smaller factor, flipping to improve triangle quality, and optionally relaxing tangentially.

The split and collapse thresholds SHALL be separated by hysteresis, so that a stationary brush cannot oscillate one edge between the two.

The target edge length SHALL be expressible RELATIVE TO THE BRUSH as well as in world units, so that a smaller brush produces finer geometry without a second control.

The number of topology operations per stamp SHALL be bounded, and the bound SHALL be a parameter rather than a constant, so a host can trade detail for latency.

Remeshing SHALL be schedulable before the deformation, after it, or both, per verb. The default for each verb SHALL be recorded with its reason rather than shared.

Boundaries, UV seams and sharp edges SHALL be preserved when the settings ask for it, and a collapse SHALL NOT close a boundary loop.

#### Scenario: A stretched patch converges
- **WHEN** a stretched triangulated patch is remeshed repeatedly toward a target edge length
- **THEN** the edge-length distribution converges toward the target and the surface does not drift beyond the stated bound

#### Scenario: Hysteresis prevents oscillation
- **WHEN** a stamp is applied repeatedly without moving
- **THEN** the same edge is not alternately split and collapsed between stamps

### Requirement: Adaptive sculpting is deterministic
Applying the same verbs with the same settings to the same surface SHALL produce the same result on every run, every platform and every standard library implementation, as the fixed-topology verbs already promise.

The candidate set for topology operations SHALL be ordered by stable identity before any operator runs. An order that comes from hash-map iteration or from thread scheduling SHALL NOT be relied on: it produces a different sequence of splits on a different platform, and the resulting surface is plausible enough that no test catches it.

Where the deformation math is shared with the fixed-topology sculptor, a stamp with topology changes disabled SHALL agree with the fixed-topology result within a stated tolerance.

#### Scenario: Two runs agree
- **WHEN** the same stroke is applied twice to identical surfaces
- **THEN** the two results are bit-identical, topology included

#### Scenario: Topology off matches the fixed sculptor
- **WHEN** a stamp runs on a dynamic surface with splitting, collapsing and flipping disabled, and the same stamp runs through the fixed-topology sculptor on the same mesh
- **THEN** the resulting positions agree within the stated tolerance

### Requirement: A dab costs what it touches
Spatial queries, topology mutation, normal recomputation and index maintenance SHALL be local to the brush footprint.

An ordinary stamp SHALL NOT rebuild the whole spatial index, SHALL NOT rebuild whole-surface adjacency, and SHALL NOT copy the whole surface for preview.

For a fixed brush footprint, stamp cost SHALL remain within one band as the total surface grows across at least two orders of magnitude, allowing for logarithmic traversal and cache effects.

A topology mutation SHALL mark the spatial partitions it changed and no others.

#### Scenario: Cost follows the footprint, not the model
- **WHEN** the same brush footprint is stamped on surfaces of 100k, 1M and 5M triangles
- **THEN** the stamp cost stays within one band rather than scaling with the total triangle count

#### Scenario: No global rebuild on an ordinary dab
- **WHEN** an ordinary stamp with topology changes runs
- **THEN** the instrumentation records no whole-surface adjacency build and no whole-index rebuild

### Requirement: A topology gesture is one sparse undo step
The library SHALL record the connectivity, geometry and attribute changes a gesture made as a reversible delta, and SHALL NOT snapshot the surface.

The record SHALL be COALESCED over the gesture: one entry per element, keeping the first `before` and the last `after`, so its size follows the elements touched rather than the stamps taken.

Reverting SHALL restore the surface BIT-EXACTLY, connectivity included, and reverting then re-applying SHALL each be idempotent.

The delta SHALL encode and decode through a versioned format whose decoder REJECTS hostile or truncated counts before allocating.

One gesture SHALL be one undo step even when it contains hundreds of stamps and thousands of topology operations, and a step spanning a scene command and a topology delta SHALL undo as one.

#### Scenario: A stroke reverts exactly
- **WHEN** a stroke that split, collapsed and flipped many edges is reverted
- **THEN** the surface is bit-identical to before the stroke, connectivity included

#### Scenario: Undo size follows what was touched
- **WHEN** a long gesture stamps repeatedly over one small region
- **THEN** the delta records each affected element once, and its size does not grow with the number of stamps

### Requirement: A host is told what changed, not handed the whole surface
The library SHALL expose the surface's changed partitions with separate revisions for topology, geometry and attributes, so a host re-uploads an index buffer only when connectivity changed and vertex data only when it moved.

Readback SHALL support caller-owned buffers with a capacity query. The library SHALL NOT allocate a heap object per changed partition per frame.

A whole-surface export SHALL remain available for correctness, and the changed-partition path SHALL be tested against it.

Borrowed pointers into mutable storage SHALL NOT be offered where a mutation can invalidate them without a generation or revision the caller can check first.

#### Scenario: A stamp reports only its own partitions
- **WHEN** a stamp changes one region of a large surface
- **THEN** the changed-partition list names the partitions covering that region and no others

#### Scenario: The dirty path and the whole export agree
- **WHEN** a stroke is applied and the surface is reconstructed from the changed-partition stream
- **THEN** the reconstruction equals the whole-surface export
