# ClayCore — Dynamic Topology / Dyntopo + Mobile Threads / QoS

**Detailed implementation guide for the current ClayCore `main` branch**  
**Repository:** `CyberdyneCorp/ClayCore`  
**Audit date:** 2026-09-01  
**Primary targets:** adaptive mesh sculpting correctness/completion + iPad interactive scheduling/latency

---

## 0. Executive summary

The current repository is farther along on Dynamic Topology than the roadmap status alone implies.

The C++ core already contains most of the architecture that a correct Dyntopo implementation needs:

- `mesh::DynamicSurface`
- stable generational topology handles
- half-edge connectivity
- vertex and corner attribute domains
- edge constraints
- split / collapse / flip topology operators
- brush-relative local remeshing
- `mesh::DynamicBvh`
- local dirty tracking
- shared brush kernels/worksets/automasking
- `mesh::DynamicSculptor`
- sparse `TopologyDelta`
- local normal recomputation
- serialization and revision tracking

So the correct development strategy is **not to rewrite Dynamic Topology**. The team should complete, verify, expose, benchmark, and harden the current implementation.

The Mobile Threads / QoS side is less complete. ClayCore already has a shared process-wide thread pool in:

```text
include/clay/parallel/thread_pool.h
```

and cooperative cancellation in:

```text
include/clay/parallel/cancel.h
```

The thread pool has already improved in two important ways:

1. nested calls execute inline instead of corrupting/replacing the current pool job;
2. the issuing thread now sleeps on a condition variable instead of spinning on `yield()`.

However, the remaining roadmap concern is still valid:

```text
hardware_concurrency() - 1 workers
+ no workload class
+ no Apple QoS assignment
+ no distinction between Pencil-critical and background work
+ no heterogeneous-core policy
```

On iPad that can allow maintenance, meshing, conversion, or other utility work to compete with the interactive brush path.

The two projects therefore meet at one central contract:

> **Interactive sculpting must remain latency-bounded even while topology adaptation and background ClayCore work are active.**

The development milestone should be:

```text
DynamicSurface stroke
    ↓
local remesh
    ↓
shared brush kernel
    ↓
local normal/BVH update

executed under

Interactive scheduling policy

while

Utility / Background jobs
do not steal the latency budget
```

---

# 1. Current architecture confirmed in `main`

## 1.1 Dynamic topology source areas

```text
include/clay/mesh/dynamic_surface.h
src/mesh/dynamic_surface.cpp

include/clay/mesh/dynamic_sculpt.h
src/mesh/dynamic_sculpt.cpp

include/clay/mesh/dynamic_bvh.h
src/mesh/dynamic_bvh.cpp

include/clay/mesh/remesh_local.h
src/mesh/remesh_local.cpp

include/clay/mesh/topology_ops.h
src/mesh/topology_ops.cpp

include/clay/mesh/topology_delta.h
src/mesh/topology_delta.cpp

include/clay/mesh/slot_pool.h
```

Shared sculpt infrastructure:

```text
include/clay/mesh/sculpt_common.h
include/clay/mesh/sculpt_kernels.h
include/clay/mesh/sculpt_workset.h
include/clay/mesh/brush_arena.h
include/clay/mesh/automask.h
```

## 1.2 Current parallel infrastructure

```text
include/clay/parallel/thread_pool.h
include/clay/parallel/cancel.h
```

`thread_pool.h` is intentionally the one low-level data-parallel primitive. It currently:

- owns a process-wide persistent pool;
- splits `[0, n)` into contiguous chunks;
- over-decomposes work to allow faster cores to claim more chunks;
- runs nested calls inline;
- lets the calling thread participate;
- sleeps during the final join using a condition variable;
- creates `hardware_concurrency() - 1` worker threads;
- has no QoS/work-class abstraction.

That last point is the main Mobile QoS gap.

---

# PART I — DYNAMIC TOPOLOGY / DYNTOPO

# 2. Representation boundary

Do not put adaptive topology inside `MeshSculptor`.

Keep:

```text
mesh::Mesh
    │
    ├── MeshSculptor
    │     fixed topology
    │
    └── MultiresSurface
          deterministic hierarchy

mesh::DynamicSurface
    │
    └── DynamicSculptor
          mutable topology
```

The fixed-mesh guarantee is valuable:

```text
indices and quads remain unchanged
```

after ordinary fixed-mesh sculpting.

Dynamic topology should stay explicit.

---

# 3. Why DynamicSurface is a separate representation

A mutable sculpting surface needs properties that the ordinary flat `Mesh` cannot safely provide:

```text
stable identity
mutable adjacency
local insertion/deletion
topology-aware undo
incremental spatial index
constraint-aware operators
```

A flat array index such as:

```cpp
vertices[12345]
```

cannot be treated as permanent identity after split/collapse operations.

ClayCore's current generational slot approach is the correct design:

```text
VertexId = { slot, generation }
FaceId   = { slot, generation }
...
```

Example:

```text
slot 42, generation 6 = old vertex
delete
slot 42 reused
slot 42, generation 7 = new vertex
```

The old handle remains invalid.

This protects:

- undo records;
- BVH references;
- selections;
- topology work queues;
- host-side cached references.

---

# 4. DynamicSurface topology model

Current `DynamicSurface` is based on a half-edge structure.

Conceptually:

```cpp
struct DynamicVertex {
    cfloat3 position;
    cfloat3 normal;

    cfloat3 color;
    float mask;

    HalfEdgeId outgoing;
    uint32_t flags;
};

struct DynamicHalfEdge {
    VertexId origin;
    FaceId face;
    HalfEdgeId next;
    HalfEdgeId twin;
    EdgeId edge;

    cfloat2 uv;
};

struct DynamicEdge {
    HalfEdgeId halfedge;
    uint32_t constraints;
};

struct DynamicFace {
    HalfEdgeId halfedge;
    cfloat3 normal;
    uint32_t flags;
};
```

This should remain authoritative.

---

# 5. Attribute domains

The current design correctly separates:

```text
vertex-domain attributes
corner-domain attributes
```

For example:

```text
position → vertex
normal   → vertex/derived
color    → vertex
mask     → vertex

UV       → corner / half-edge
```

This matters because a UV seam may be:

```text
one geometric vertex
two independent UV values
```

If Dyntopo stored UV only per geometric vertex, a split/collapse near a seam would either weld the UVs or crack the geometry.

Do not simplify this model.

---

# 6. Edge constraints

Current `DynamicSurface` defines the important constraint classes:

```text
Boundary
UvSeam
Sharp
Material
UserLocked
```

The architectural decision is correct:

> constraints live on the edge and are enforced by the topology operator itself.

Do not depend on the remesher to pre-filter correctly.

Every public/internal topology operator must remain safe when called independently.

---

# 7. Topology operator 1 — split

## 7.1 Goal

Split an edge that is too long relative to the target local edge length.

```text
A -------- B
```

becomes:

```text
A ---- M ---- B
```

For a two-triangle interior edge, adjacent triangles are replaced with four triangles.

## 7.2 New vertex interpolation

For a midpoint split:

```cpp
new_vertex.position =
    lerp(a.position, b.position, 0.5f);

new_vertex.color =
    lerp(a.color, b.color, 0.5f);

new_vertex.mask =
    lerp(a.mask, b.mask, 0.5f);
```

Normals are derived and should be recomputed locally rather than authored by interpolation alone.

## 7.3 Corner attributes

UV interpolation must happen independently on both incident face sides.

Pseudo:

```cpp
new_corner_left.uv =
    lerp(left_a.uv, left_b.uv, 0.5f);

new_corner_right.uv =
    lerp(right_a.uv, right_b.uv, 0.5f);
```

On a UV seam:

```text
new_corner_left.uv != new_corner_right.uv
```

is completely valid.

## 7.4 Constraint propagation

Splitting constrained features should preserve them.

Example:

```text
A ======== B    Sharp
```

becomes:

```text
A ==== M ==== B
```

with both new edges remaining Sharp.

Similar propagation should apply to UV seam/material/boundary policy where appropriate.

## 7.5 Atomicity

Keep the current operator rule:

```text
DECIDE
  ↓
validate all preconditions
  ↓
reserve/allocate required topology
  ↓
WRITE
```

Do not partially rewire and then attempt rollback.

---

# 8. Topology operator 2 — collapse

Collapse is more dangerous because it can change manifoldness.

## 8.1 Basic operation

```text
A --- B
```

becomes one surviving vertex.

Incident triangles that become degenerate disappear and the surrounding fan is rewired.

## 8.2 Link condition

For an interior edge `(u,v)`, the neighborhood intersection must match the expected opposite vertices of the incident triangles.

Conceptually:

```cpp
intersection(neighbors(u), neighbors(v))
```

must not contain unexpected vertices.

Without this check, a collapse can:

- merge unrelated surface sheets;
- create duplicate triangles;
- produce non-manifold topology.

## 8.3 Geometric refusal tests

Before committing:

```text
no resulting triangle may have near-zero area
no triangle may invert
normal change must remain within policy
no duplicate face may be created
```

## 8.4 Constraint refusal

Collapse should normally refuse if it would destroy:

```text
Boundary
UvSeam
Sharp
Material
UserLocked
```

unless an explicit operation policy says otherwise.

## 8.5 Position policy

A deterministic P0 position is sufficient:

```cpp
Vec3 merged =
    0.5f * (a.position + b.position);
```

Do not block Dyntopo completion on QEM.

A later quality pass may add:

```text
QEM
curvature-aware placement
surface-projected placement
```

after measurements justify it.

---

# 9. Topology operator 3 — flip

A flip changes a diagonal between two adjacent triangles without changing vertex count.

Use it to improve triangle shape and valence.

Only flip when:

```text
edge is unconstrained
new diagonal does not already exist
both resulting triangles are valid
orientation is preserved
quality improves
```

A simple quality measure:

```text
maximize minimum triangle angle
```

or a valence/shape hybrid.

Pseudo:

```cpp
float q_before = min_triangle_quality(t0, t1);
float q_after  = min_triangle_quality(t0_flipped, t1_flipped);

if (q_after <= q_before + epsilon)
    return NoImprovement;
```

Tie-breaking must use stable IDs.

---

# 10. Local remeshing algorithm

Current `remesh_local.*` already implements the intended policy:

```text
under brush
    ↓
split long edges
    ↓
collapse short edges
    ↓
flip bad diagonals
    ↓
tangential relax
```

This should remain local to the brush.

Do not turn it into a full-surface remesh.

---

# 11. Brush-relative detail

Current settings correctly default to:

```cpp
target_edge_length =
    brush_radius / detail_resolution;
```

Example:

```text
brush radius        = 24 mm
detail resolution   = 8
target edge length  = 3 mm
```

After shrinking the brush:

```text
brush radius        = 4 mm
detail resolution   = 8
target edge length  = 0.5 mm
```

This is the right sculpting behavior.

The user does not need to manage two unrelated sliders just to get finer triangles with a finer brush.

---

# 12. Split/collapse hysteresis

Current defaults are approximately:

```cpp
split_factor    = 1.33f;
collapse_factor = 0.8f;
```

Keep the gap.

Without it:

```text
edge slightly too long
    ↓
split
    ↓
children slightly too short
    ↓
collapse
    ↓
repeat forever
```

Hysteresis makes a stationary brush converge.

Acceptance test:

```text
hold identical brush stamp repeatedly
```

and require:

```text
topology operation count approaches zero
```

after convergence.

---

# 13. Bounded remesh work per dab

Current settings already expose:

```cpp
max_passes = 3;
max_ops_per_stamp = 4096;
```

This is important for interactive latency.

Do not allow local remeshing to iterate until mathematical convergence.

Better:

```text
dab 1  → partially adapts
dab 2  → improves more
dab 3  → converges
```

than:

```text
dab 1 → 40 ms stall
```

Add instrumentation for:

```text
split count
collapse count
flip count
relax count
refusal counts
hit_budget
```

`hit_budget` is particularly valuable to host tuning.

---

# 14. Remesh timing per brush

Do not use one timing for all verbs.

## Before deformation

Recommended for brushes that need topology available to represent incoming detail:

```text
Draw
Clay
Crease
Inflate
Flatten
Pinch
Smooth
Polish
Scrape
Nudge
```

Pipeline:

```text
remesh
  ↓
deform
```

## After deformation

For stretch-producing operations:

```text
Grab
Relax
```

Pipeline:

```text
deform
  ↓
stretched geometry
  ↓
remesh
```

## Before and after

For SnakeHook-style behavior:

```text
remesh
  ↓
pull
  ↓
remesh stretched region
```

Keep this policy centralized in `default_timing(MeshBrush)`.

---

# 15. DynamicSculptor

The current `DynamicSculptor` already has the correct representation responsibilities:

```text
gather local region
walk mutable adjacency
construct shared workset
apply shared brush kernel
run remesher
update local normals
maintain DynamicBvh
record TopologyDelta
```

Do not duplicate brush math inside it.

The shared runtime should remain the source of:

```text
falloff
alpha
mask
automask
brush frame
strength
brush deformation formula
```

DynamicSculptor should own only the DynamicSurface-specific mechanics.

---

# 16. Dynamic `Layer` brush

Current code intentionally does not offer one of the fixed-mesh verbs: Layer.

This is correct.

The fixed Layer brush depends on the position of each vertex at stroke start.

After Dyntopo splitting:

```text
many current vertices did not exist at stroke start
```

so there is no valid per-vertex reference.

Do not implement:

```text
old vertices → Layer behavior
new vertices → Draw behavior
```

That creates one artist verb with two semantics.

Possible future solution:

```text
snapshot/reference surface at stroke start
           ↓
closest surface point
           ↓
reference height for new vertices
```

But keep it out of the Dyntopo completion milestone unless explicitly designed.

---

# 17. Complete stroke integration

A dab is one brush application.

A stroke is the entire Pencil/mouse drag.

The artist-facing integration should be:

```text
StrokeSample[]
     ↓
stroke resolver
     ↓
Stamp[]
     ↓
DynamicSculptor::stamp(...)
     ↓
one TopologyDelta for the whole stroke
```

Critical history rule:

> 150 dabs in one Pencil stroke = one undo step.

Pseudo:

```cpp
TopologyDelta delta;

for (const auto& stamp : resolved_stroke.stamps) {
    MeshBrushSettings b = preset.settings;

    b.center = stamp.position;
    b.radius = stamp.radius;
    b.strength = stamp.strength;
    b.stamp_azimuth = stamp.azimuth;

    sculptor.stamp(
        verb,
        b,
        topology,
        gate,
        &delta);
}

session_history.record_dynamic_surface(
    surface_id,
    std::move(delta));
```

If the current public/C ABI path still exposes only individual stamps, add a whole-stroke convenience path rather than forcing every host to reproduce transaction semantics.

---

# 18. DynamicBvh

Current DynamicBvh is a strong foundation.

Its design intentionally uses chunked leaves rather than a per-face tree.

Current default policy:

```text
target leaf ≈ 256 faces
split above ≈ 512
merge threshold ≈ 64
```

This chunk is intended to serve simultaneously as:

```text
BVH leaf
brush candidate set
parallel work unit
normal update unit
dirty tracking unit
host upload unit
```

Preserve that concept.

---

# 19. Incremental BVH maintenance

Geometry-only edit:

```text
moved vertex
    ↓
incident faces
    ↓
affected leaves
    ↓
recompute leaf bounds
    ↓
refit ancestors
```

Topology edit:

```text
created/deleted/rewired faces
    ↓
insert / erase / update
    ↓
affected chunks only
```

Prefer `update_many()` over one `update()` per face.

Bad:

```cpp
for (FaceId f : dirty_faces)
    bvh.update(surface, f);
```

Better:

```cpp
bvh.update_many(surface, dirty_faces);
```

The batched version can deduplicate leaves and ancestors.

---

# 20. BVH quality degradation

An incrementally refitted tree stays correct but may become spatially poor.

Current API already provides:

```cpp
quality()
wants_rebuild()
```

Keep the important behavioral rule:

> full rebuild happens between strokes, never during the middle of a drag.

A rebuild during one random dab produces unpredictable latency spikes.

Later, Mobile QoS should classify this rebuild as Utility work.

---

# 21. Dirty leaves and host updates

Current DynamicBvh already has:

```cpp
dirty_leaves()
clear_dirty()
```

using epoch-based marking instead of a hash set.

This is exactly the right direction.

The remaining product task is to ensure this becomes a complete host-visible dirty-chunk contract.

The host should be able to do:

```text
stroke
  ↓
query dirty chunks
  ↓
check geometry/topology/attribute revisions
  ↓
copy only changed chunk buffers
  ↓
update only those GPU ranges
```

rather than:

```text
DynamicSurface::to_mesh()
  ↓
copy complete model
  ↓
upload complete model
```

---

# 22. Separate revision domains

`DynamicStampResult` already reports:

```text
topology_revision
geometry_revision
attribute_revision
dirty_bounds
```

Take this idea to chunk level.

Why:

## Position-only deformation

Requires:

```text
position upload
normal upload
```

Does not require:

```text
index upload
```

## Split/collapse/flip

Requires:

```text
position
normal
index
corner-domain data
```

## Mask/color change

May need only:

```text
attribute stream
```

Proposed chunk state:

```cpp
struct SurfaceChunkRevision {
    uint64_t geometry = 0;
    uint64_t topology = 0;
    uint64_t attributes = 0;
};
```

---

# 23. Local normal recomputation

Never globally recompute normals after a local adaptive dab.

Algorithm:

```text
moved vertices
+
created/deleted/rewired faces
      ↓
dirty face set
      ↓
dirty vertex set / one-ring
      ↓
recompute dirty face normals
      ↓
recompute dirty vertex normals
```

Use generation-stamped vectors/bitsets rather than allocating `unordered_set` every dab.

Example:

```cpp
class DirtyMarks {
public:
    void begin_epoch() {
        ++epoch_;
        dirty_.clear();
    }

    void mark(uint32_t slot) {
        if (marks_[slot] == epoch_)
            return;

        marks_[slot] = epoch_;
        dirty_.push_back(slot);
    }

private:
    uint32_t epoch_ = 1;
    std::vector<uint32_t> marks_;
    std::vector<uint32_t> dirty_;
};
```

This gives near-zero clearing cost.

---

# 24. Sparse TopologyDelta undo

Keep the existing sparse topology history model.

A stroke should record only touched elements:

```text
created
deleted
rewritten
```

for:

```text
vertices
half-edges
edges
faces
```

Coalesce repeated edits:

```text
first before-state
last after-state
```

rather than recording every intermediate remesh micro-step.

Undo correctness requires restoring:

```text
positions
attributes
connectivity
slot occupancy
generation values
constraints
```

not merely the visible shape.

---

# 25. Dyntopo determinism

The roadmap explicitly requires determinism to be stated, not assumed.

Potential sources of nondeterminism:

```text
unordered_map iteration
unordered_set iteration
parallel topology mutations
equal-length edge choices
equal-quality flip choices
slot reuse order
BVH partition ties
floating reduction order
```

Recommended rule:

```text
parallel gather
    ↓
stable IDs
    ↓
sort deterministically
    ↓
serial topology commit
```

at least for the first production version.

Tie-break using stable IDs.

Example:

```cpp
std::sort(edges.begin(), edges.end(),
    [&](EdgeId a, EdgeId b) {
        float la = edge_length(a);
        float lb = edge_length(b);

        if (la != lb)
            return la > lb;

        return a.raw() < b.raw();
    });
```

Do not parallelize split/collapse/flip mutation until conflict scheduling has explicit deterministic semantics.

---

# 26. Dyntopo validation suite

Add a topology torture validator that checks after random operations:

```text
every live face has exactly 3 corners
half-edge next cycle closes in 3
interior twins agree
face ownership agrees
edge ownership agrees
no reference points to a dead handle
no duplicate triangle
no invalid generation
no non-manifold edge
no zero-area triangle
finite coordinates
finite normals
consistent winding
constraints remain valid
```

Random deterministic sequence:

```text
split
collapse
flip
split
brush dab
grab
snakehook
undo
redo
serialize
deserialize
continue
```

Run under:

```text
ASan
UBSan
release
```

---

# 27. Dyntopo C ABI completion

Audit `bindings/c/clay.h` against all current C++ capability.

Required host capabilities should include:

```text
create DynamicSurface from Mesh
destroy surface
serialize/deserialize
validate/stats/memory
create DynamicSculptor
set topology settings
perform stamp
perform whole stroke
query dirty bounds
query dirty chunks/leaves
query revisions
copy chunk geometry
request index rebuild between strokes
convert to Mesh
undo/history integration
```

Any host-visible feature absent from C and pyclay is not complete.

---

# 28. Dyntopo Python bindings

Desired Python usage:

```python
surface = clay.DynamicSurface.from_mesh(mesh)

sculptor = clay.DynamicSculptor(surface)

with session.group("SnakeHook"):
    sculptor.stroke(
        brush="snakehook",
        samples=samples,
        detail_mode="brush_relative",
        detail_resolution=10.0,
    )
```

Expose diagnostics:

```python
result.moved_vertices
result.remesh.split
result.remesh.collapsed
result.remesh.flipped
result.remesh.hit_budget
result.dirty_bounds
```

and optionally:

```python
sculptor.dirty_chunks()
```

for host/runtime testing.

---

# PART II — MOBILE THREADS / QoS

# 29. Problem statement

ClayCore's current pool is a correct general data-parallel primitive but not yet an interactive scheduling policy.

Current constructor conceptually does:

```cpp
unsigned hc = std::thread::hardware_concurrency();
unsigned count = hc > 1 ? hc - 1 : 0;

for (...)
    threads_.emplace_back(...);
```

This means the pool currently assumes:

```text
all logical cores are equivalent
all ClayCore jobs deserve the same priority
one global worker count is appropriate
```

These assumptions are weak on Apple mobile devices.

Apple SoCs contain performance and efficiency cores, and QoS informs the OS scheduler how urgently work should run.

ClayCore needs to communicate:

```text
this work is required for the Pencil event

versus

this work may finish later
```

---

# 30. Good things already present in `thread_pool.h`

Do not discard the improvements already landed.

## 30.1 Persistent pool

Worker creation is amortized.

Correct.

## 30.2 Over-decomposition

The pool creates roughly multiple chunks per worker instead of one static chunk.

This allows fast workers to claim extra work.

Correct and especially relevant for heterogeneous cores.

## 30.3 Nested parallel calls execute inline

Current behavior prevents a nested `parallel_for` from replacing the outer current job.

Preserve this unless the scheduler becomes a true multi-job task system.

## 30.4 Join sleeps

The old `yield()` spin has already been replaced with a `condition_variable`.

Do not regress this.

---

# 31. Scheduling objective

We do not need a giant general-purpose task graph.

ClayCore primarily needs:

```text
work classification
QoS mapping
worker policy
latency isolation
```

Keep the API small.

---

# 32. Proposed work classes

Add:

```cpp
namespace clay::parallel {

enum class WorkClass : uint8_t {
    Interactive,
    UserInitiated,
    Utility,
    Background
};

}
```

Interpretation:

## Interactive

Work required for the current visible interaction.

Examples:

```text
current sculpt dab
local Dyntopo remesh required by current dab
local BVH refit
local normal recompute
mask evaluation needed for current dab
```

## UserInitiated

Explicit operation the user is waiting for, but not a per-frame event.

Examples:

```text
subdivide Multires
global remesh
convert representation
explicit consolidate
large mask operation
```

## Utility

Useful derived work that may lag slightly.

Examples:

```text
DynamicBvh quality rebuild between strokes
cache refill
background meshing
redistance
non-critical evaluation
```

## Background

No immediate visual dependency.

Examples:

```text
preparing autosave data
cache housekeeping
optional statistics
non-urgent serialization preparation
```

---

# 33. Do not expose raw Apple QoS in the portable API

Bad portable API:

```cpp
parallel_for(..., QOS_CLASS_USER_INTERACTIVE);
```

ClayCore should not make public/core modules depend on Apple constants.

Use:

```cpp
parallel::WorkClass::Interactive
```

and map inside an Apple implementation.

---

# 34. Proposed ThreadPool API evolution

Keep the old call working.

Existing:

```cpp
for_range(n, min_chunk, fn);
```

Add:

```cpp
for_range(
    WorkClass work_class,
    std::size_t n,
    std::size_t min_chunk,
    const RangeFn& fn);
```

or options:

```cpp
struct ParallelForOptions {
    WorkClass work_class = WorkClass::UserInitiated;
    std::size_t min_chunk = 1;
};

void for_range(
    std::size_t n,
    const ParallelForOptions& options,
    const RangeFn& fn);
```

Prefer options if more policy fields are likely.

Compatibility:

```cpp
for_range(n, min_chunk, fn)
```

can initially map to:

```text
UserInitiated
```

to avoid silently turning all existing work into UI priority.

---

# 35. One pool or multiple pools?

There are two plausible designs.

## Option A — separate pools/queues by class

```text
Interactive pool
Utility pool
Background pool
```

Advantages:

```text
simple QoS assignment
clear worker limits
strong isolation
```

Disadvantages:

```text
more threads
possible oversubscription
idle workers duplicated
```

## Option B — one scheduler with worker classes

```text
scheduler
 ├─ high-priority queue
 ├─ utility queue
 └─ background queue
```

Advantages:

```text
less oversubscription
workers can help across classes
```

Disadvantages:

```text
more complex
existing ThreadPool is explicitly not a task system
```

### Recommendation

For ClayCore's current architecture, use a **minimal two-lane design** rather than turning `ThreadPool` into a full task scheduler:

```text
Interactive/UserInitiated lane
Utility/Background lane
```

or keep synchronous `parallel_for` and assign a **per-call requested WorkClass** to workers participating in that generation.

The lowest-risk first version is:

> keep the existing synchronous single-job pool, add QoS-aware worker configuration and work-class policy, then add a second maintenance lane only if benchmarks prove priority inversion remains.

---

# 36. Apple QoS mapping

On Apple:

```text
Interactive   → USER_INTERACTIVE
UserInitiated → USER_INITIATED
Utility       → UTILITY
Background    → BACKGROUND
```

Exact primitive may be implemented using:

```text
pthread QoS APIs
Grand Central Dispatch
```

For the existing `std::thread` pool, `pthread` QoS assignment is the most contained adaptation.

Keep Apple-specific code outside generic headers.

Suggested files:

```text
include/clay/parallel/work_class.h

src/parallel/platform_thread_policy.h
src/parallel/platform_thread_policy.cpp

src/parallel/apple_thread_policy.mm
```

or if the library remains header-heavy:

```text
include/clay/parallel/detail/platform_thread_policy.h
src/parallel/apple_thread_policy.cpp
```

---

# 37. Important nuance: thread QoS vs work QoS

A persistent worker has a thread QoS.

But calls may alternate:

```text
Interactive dab
Utility rebuild
Interactive dab
```

Therefore setting all workers permanently to UserInteractive is wrong.

Possible solutions:

## Strategy 1 — set worker QoS per job generation

When worker wakes:

```cpp
apply_thread_policy(job.work_class);
run(job);
restore_default_policy();
```

Pseudo:

```cpp
void worker() {
    for (;;) {
        auto job = wait_for_job();

        ScopedThreadWorkClass qos(job->work_class);
        run(*job);
    }
}
```

This is the most direct adaptation to the current ThreadPool.

## Strategy 2 — dedicated QoS pools

Only if per-job QoS switching is measured to be problematic.

Start with Strategy 1.

---

# 38. Calling-thread participation

Current `parallel_for()` lets the caller execute chunks too:

```cpp
run(*job);
```

This is generally efficient.

But the caller may be the UI/Pencil thread.

We need to define the rule explicitly.

For Interactive work:

```text
calling thread participates
```

is often desirable because the caller is already blocked waiting for result.

For Utility work invoked from UI:

```text
the host should not execute it synchronously on the UI thread
```

That is primarily a host usage rule unless ClayCore grows asynchronous operations.

Document:

> QoS does not make a synchronous Utility call non-blocking. A host must still dispatch long synchronous ClayCore calls off its UI thread.

---

# 39. Worker count policy

Do not blindly treat:

```cpp
hardware_concurrency() - 1
```

as the final mobile policy.

Introduce configuration.

```cpp
struct ThreadPoolConfig {
    unsigned max_workers = 0; // 0 = automatic

    unsigned interactive_workers = 0;
    unsigned utility_workers = 0;

    bool reserve_caller_capacity = true;
};
```

However, avoid exposing an overly complicated policy before measurements.

A simpler first public/internal config may be:

```cpp
struct ThreadPoolConfig {
    unsigned worker_limit = 0; // 0 auto
};
```

plus platform-derived auto settings.

---

# 40. Heterogeneous Apple CPUs

`hardware_concurrency()` does not tell ClayCore which cores are P or E cores.

Do not implement hard-coded chip tables.

Prefer OS scheduling policy through QoS and conservative worker limits.

The OS is better positioned to map:

```text
USER_INTERACTIVE
```

to suitable resources.

Over-decomposition already helps because faster cores can claim more chunks.

Thus:

```text
QoS + dynamic chunk claiming
```

is preferable to trying to manually bind every worker to a specific core.

---

# 41. Avoid thread affinity on iOS unless proved necessary

Do not initially implement:

```text
pin interactive worker 0 to P-core X
```

Reasons:

```text
not portable
OS thermal decisions may conflict
Apple scheduler already understands topology
chip-specific assumptions age quickly
```

Use QoS first.

Only explore affinity if device profiling proves QoS insufficient.

---

# 42. Priority inversion

Potential problem:

```text
Utility job starts
    ↓
occupies all ClayCore workers
    ↓
Pencil event arrives
```

The current pool supports only one advertised current job, so it is not designed for preemption.

This means work classification alone does not solve every case.

## First mitigation

ClayCore's host should avoid running long Utility operations concurrently with interactive sculpting unless explicitly supported.

## Engine mitigation

Break utility work into bounded chunks and cancellation/checkpoint boundaries.

Example:

```text
rebuild 1000 chunks
```

instead of one monolithic call:

```text
rebuild whole world with no checkpoint
```

## Future scheduler upgrade

If concurrent maintenance during sculpt becomes a requirement, add:

```text
high-priority pending job
```

and allow utility workers to yield at chunk boundaries.

Do not build this complexity before a benchmark demonstrates the need.

---

# 43. Recommended job-yield design if needed

Future extension:

```cpp
struct Job {
    WorkClass work_class;
    std::atomic<size_t> next;
    ...
};
```

Worker loop:

```cpp
while (claim_chunk(job)) {
    run_chunk();

    if (job.work_class >= WorkClass::Utility &&
        scheduler.has_pending_interactive()) {
        requeue_remaining(job);
        return;
    }
}
```

Important:

```text
yield only at chunk boundaries
```

Never interrupt a topology operator halfway through mutation.

---

# 44. DynamicTopology scheduling classification

Classify Dyntopo work carefully.

## Interactive

Inside current dab:

```text
DynamicBvh query
region traversal
remesh required to execute current dab
shared brush kernel
local normal update
incremental BVH maintenance
dirty chunk preparation
```

All of these are on the current visible critical path.

## Utility

Between-stroke maintenance:

```text
full DynamicBvh rebuild after quality decay
optional cache compaction
non-critical statistics
```

## UserInitiated

Explicit whole-surface operations:

```text
DynamicSurface conversion
validation over entire surface
global remesher
```

---

# 45. Cancellation

Current `CancelToken` is already correctly designed around plain atomics.

Preserve these rules:

```text
host may cancel from another thread
engine polls cancellation
cancelled worker chunk RETURNS NORMALLY
no exception escapes worker function
```

This is mandatory because the thread pool increments job completion only after the range callback returns.

Throwing from a worker on cancel can make the issuing thread wait forever.

---

# 46. Cancellation and Dyntopo

A normal per-dab Dyntopo pass should be small enough that it does not need host cancellation.

Use bounded remesh budget instead.

Cancellation is more relevant to:

```text
global conversion
global validation
full rebuild
large import/export
global remesher
Multires subdivision
```

Do not add atomic-cancel checks inside every split operation unless necessary.

The correct latency control for interactive Dyntopo is:

```text
bounded work per dab
```

not a Cancel button.

---

# 47. QoS RAII helper

Suggested platform-neutral wrapper:

```cpp
namespace clay::parallel {

class ScopedWorkClass {
public:
    explicit ScopedWorkClass(WorkClass cls)
        : previous_(platform_current_work_class()) {
        platform_set_work_class(cls);
    }

    ~ScopedWorkClass() {
        platform_set_work_class(previous_);
    }

private:
    WorkClass previous_;
};

}
```

Worker:

```cpp
void worker() {
    for (;;) {
        std::shared_ptr<Job> job = wait_for_job();

        if (!job)
            continue;

        ScopedWorkClass work_class(job->work_class);
        run(*job);
    }
}
```

The actual Apple implementation may need a different restore mechanism; keep the public abstraction stable.

---

# 48. ThreadPool Job changes

Current job approximately contains:

```cpp
struct Job {
    std::function<void(size_t, size_t)> fn;
    size_t n;
    size_t chunk;
    size_t num_tasks;
    atomic<size_t> next;
    atomic<size_t> done;
    mutex;
    condition_variable;
};
```

Extend with:

```cpp
WorkClass work_class =
    WorkClass::UserInitiated;
```

Potential future diagnostics:

```cpp
uint64_t generation;
uint64_t submit_timestamp_ns;
```

Do not add expensive tracing to release builds unless behind instrumentation.

---

# 49. Proposed updated parallel_for

Example:

```cpp
void parallel_for(
    WorkClass work_class,
    std::size_t n,
    std::size_t min_chunk,
    const RangeFn& fn)
{
    if (n == 0)
        return;

    if (in_job()) {
        fn(0, n);
        return;
    }

    ...

    auto job = std::make_shared<Job>();

    job->work_class = work_class;
    job->fn = fn;
    ...

    publish(job);

    {
        ScopedWorkClass caller_class(work_class);
        run(*job);
    }

    wait_for_completion(job);
}
```

Nested inline calls inherit the current thread's active QoS naturally.

---

# 50. Default work-class migration

Do not require every call site to change in one commit.

Step 1:

```cpp
for_range(n, min_chunk, fn)
```

remains.

Internally forward to:

```cpp
for_range(
    WorkClass::UserInitiated,
    n,
    min_chunk,
    fn);
```

Step 2: migrate measured interactive sites explicitly.

Step 3: migrate known background/utility sites.

Step 4: CI/lint audit can eventually reject unclassified high-cost call sites.

---

# 51. Call-site audit

Search every current use of:

```text
parallel::for_range
ThreadPool::instance()
parallel_for
```

Build a table:

| Subsystem | Operation | Class | Reason |
|---|---|---|---|
| Mesh sculpt | current dab work | Interactive | Pencil latency |
| Dynamic remesh | current dab | Interactive | Required for visible result |
| Dynamic BVH rebuild | between stroke rebuild | Utility | Derived cache |
| Voxel sculpt | current dab | Interactive | Pencil latency |
| Brick cache fill for current dab | Interactive | preview critical |
| Meshing preview | depends on immediate frame | Interactive/UserInitiated | measure |
| Redistance | explicit user operation | UserInitiated | not per-frame |
| Consolidate | explicit long operation | UserInitiated | host waits |
| Autosave prep | Background | not visual |
| File export | UserInitiated | explicit user action |

Do not assign classes purely by subsystem. Assign by **operation context**.

---

# 52. Host override / configuration

ClayCore should provide a small configuration hook so ClaySpace can adapt to device/application policy.

Potential C++:

```cpp
struct ParallelConfig {
    unsigned worker_limit = 0;
};

void parallel::configure(const ParallelConfig&);
```

C ABI:

```c
typedef struct clay_parallel_config {
    uint32_t struct_size;
    uint32_t worker_limit; /* 0 = automatic */
} clay_parallel_config;

clay_result clay_parallel_configure(
    const clay_parallel_config*);
```

Be careful: the current global pool may be constructed lazily.

Define whether configuration is:

```text
must occur before first parallel operation
```

or supports live resizing.

Recommendation:

> v1 configuration is startup-only.

Live resizing adds synchronization complexity without obvious value.

---

# 53. iPad performance budgets

The roadmap records the interactive budget approximately as:

```text
4–8 ms per Pencil event at 120–240 Hz
16.7 ms per preview frame
```

Use the exact repository device harness policy as the source of truth.

QoS success is not:

```text
threads have correct labels
```

It is:

```text
interactive P95/P99 remain inside budget under load
```

---

# 54. Device benchmark scenarios

Add/extend iPad device tests.

## Scenario A — idle system

```text
Interactive sculpt workload only
```

Measure baseline.

## Scenario B — Utility contention

```text
Interactive sculpt
+
continuous BVH/cache maintenance
```

## Scenario C — UserInitiated contention

```text
Interactive sculpt
+
large explicit operation on another host worker
```

If concurrent document access is illegal, use independent documents/surfaces.

## Scenario D — Background pressure

```text
Interactive sculpt
+
background serialization/cache work
```

Report:

```text
P50
P95
P99
max
```

for interactive dab time.

---

# 55. Scheduling acceptance criterion

A useful regression metric:

```text
P95 interactive dab under Utility load
/
P95 interactive dab idle
```

Target should remain close to 1.

Do not freeze an arbitrary number before measuring the reference iPads.

A starting engineering goal might be:

```text
< 1.25x
```

for controlled Utility contention, but benchmark first.

---

# 56. Thermal testing

A one-second benchmark is insufficient for iPad.

Test sustained behavior:

```text
5 minutes
15 minutes
30 minutes
```

because the scheduler/CPU frequency changes with thermal state.

Record:

```text
dab P95 over time
worker count
operation throughput
memory
thermal state if host/device harness exposes it
```

The ideal policy may be slower in a 500 ms desktop benchmark but better after 15 minutes.

---

# 57. Worker-count experiments

Test at least:

```text
1 worker + caller
2 workers + caller
4 workers + caller
auto
hardware_concurrency - 1
```

on reference iPads.

Do this separately for:

```text
interactive brush
heavy operation throughput
mixed contention
```

The best throughput setting may not be the best latency setting.

---

# 58. QoS and over-decomposition

Current pool uses multiple chunks per worker.

Keep this.

On heterogeneous hardware:

```text
fast worker finishes early
  ↓
claims more chunks
```

This is much better than static one-chunk-per-thread partitioning.

Tune:

```cpp
kChunksPerWorker
```

using device measurements.

Potential future per-class policy:

```text
Interactive:
smaller chunks for better balancing and shorter tail latency

Utility:
larger chunks for lower scheduling overhead
```

Example:

```cpp
size_t chunks_per_worker(WorkClass cls) {
    switch (cls) {
        case WorkClass::Interactive: return 12;
        case WorkClass::UserInitiated: return 8;
        case WorkClass::Utility: return 4;
        case WorkClass::Background: return 2;
    }
}
```

Do not adopt these numbers without benchmarks.

---

# 59. Chunk size and latency tails

Interactive scheduling is dominated by the slowest remaining chunk.

If chunks are too large:

```text
caller finishes
     ↓
waits for one E-core/slow worker
     ↓
P99 spike
```

If chunks are too small:

```text
atomic claim overhead
cache locality loss
```

Benchmark chunk granularity explicitly.

Report:

```text
median chunk duration
slowest chunk duration
claim count
worker utilization
```

---

# 60. Nested parallelism

Current inline fallback should stay.

Example:

```text
outer parallel meshing
  ↓
inner field evaluation calls parallel::for_range
```

The inner call executes serially on that worker.

This avoids corrupting the one-job pool.

If later ClayCore introduces a true multi-job scheduler, revisit this. Do not remove it simply because QoS is added.

---

# 61. No busy waiting

Current join is already fixed.

Keep:

```cpp
job.finished.wait(...)
```

Never reintroduce:

```cpp
while (!done)
    std::this_thread::yield();
```

for interactive code.

Also audit other ClayCore loops for:

```text
spin
poll without sleep
busy retry
```

especially any new QoS/maintenance implementation.

---

# 62. Interaction with background BVH rebuild

This is a concrete place Dyntopo and QoS meet.

Current policy says:

```text
DynamicBvh wants rebuild
```

is advisory.

Recommended host/engine sequence:

```text
stroke ends
    ↓
if wants_rebuild()
    ↓
schedule/request Utility rebuild
    ↓
new stroke arrives?
    ├─ no → rebuild finishes
    └─ yes → interactive work takes precedence
```

If ClayCore does not yet support safe concurrent rebuild/swap, keep rebuild synchronous **between strokes**, but run it on a host-side non-UI thread.

A future implementation can build a new immutable BVH snapshot and atomically replace between strokes.

---

# 63. Optional future: dual-BVH rebuild

Do not make this P0, but it is a good later design.

```text
active DynamicBvh A
        │
        ├─ interactive queries
        │
        └─ Utility builds B from surface snapshot
                     ↓
              stroke boundary
                     ↓
                swap A ← B
```

This only works if surface revision did not change incompatibly during rebuild or if changes can be replayed.

Do not add it until rebuild cost becomes a measured problem.

---

# 64. Mobile QoS and memory

More threads also mean more:

```text
thread stacks
scratch buffers
per-worker temporary storage
```

When choosing worker counts on iPad, measure memory.

If scratch is per sculptor rather than per worker, good.

If future per-thread scratch is introduced, include:

```text
worker_count × scratch_capacity
```

in memory accounting.

---

# 65. Mobile scheduling source layout

Recommended minimal change:

```text
include/clay/parallel/
    cancel.h
    thread_pool.h
    work_class.h

src/parallel/
    thread_policy.cpp
    apple_thread_policy.mm
```

If CMake currently treats parallel as header-only, adding one small compiled implementation is still preferable to contaminating generic headers with Apple APIs.

CMake concept:

```cmake
if(APPLE)
    target_sources(claycore PRIVATE
        src/parallel/apple_thread_policy.mm)
else()
    target_sources(claycore PRIVATE
        src/parallel/thread_policy.cpp)
endif()
```

---

# 66. Platform fallback

On non-Apple systems:

```cpp
void apply_work_class(WorkClass) noexcept {
    // no-op initially
}
```

This preserves semantics.

Later:

```text
Windows thread priority
Linux nice/pthread policy
Android scheduling hints
```

can be considered independently.

The feature should not require all platforms to implement an exact QoS equivalent on day one.

---

# 67. C ABI exposure of QoS

Do not expose internal per-job scheduling knobs everywhere.

Most C API functions should classify themselves internally.

Example:

```text
clay_dynamic_sculptor_stamp
```

is Interactive automatically.

```text
clay_sdf_consolidate
```

is UserInitiated automatically.

The host should not need:

```c
clay_dynamic_sculptor_stamp(..., CLAY_QOS_INTERACTIVE)
```

That invites misuse.

Expose only global configuration if needed:

```text
worker limit
possibly scheduling mode
```

Internal operations own their correct class.

---

# 68. Possible scheduling mode

If ClaySpace needs battery vs performance control later:

```cpp
enum class SchedulingMode {
    Automatic,
    Latency,
    Balanced,
    Efficiency
};
```

But do not include this in the first change unless requested by product requirements.

P0 is correct classification + Apple QoS + device gates.

---

# 69. Tests for WorkClass mapping

Unit test the portable layer:

```text
default old for_range → UserInitiated
explicit classes propagate into Job
nested call inherits caller/worker class
job state remains valid
condition-variable join still completes
```

Apple-specific smoke:

```text
spawn worker
run Interactive job
verify effective QoS class
```

where the platform APIs allow introspection.

Do not rely exclusively on introspection; performance tests are the real gate.

---

# 70. Concurrency correctness tests

Run:

```text
1000 repeated parallel_for calls
nested calls
0 items
1 item
less work than workers
exceptions prohibited/guarded
cancellation caller returns normally
destructor shutdown
rapid job generations
```

Use ThreadSanitizer where supported.

If TSan is incompatible with other project settings, have a dedicated preset/job.

---

# 71. Performance instrumentation

Add optional counters:

```cpp
struct ParallelStats {
    uint32_t worker_count;
    uint32_t task_count;

    uint64_t wall_ns;
    uint64_t caller_work_ns;
    uint64_t worker_work_ns;

    uint64_t slowest_chunk_ns;
};
```

For Interactive device tests, this helps distinguish:

```text
brush algorithm slow
```

from:

```text
one worker had a very slow tail
```

Keep instrumentation compiled out or lightweight in production.

---

# 72. Combined Dyntopo + QoS stress test

This is the most valuable acceptance test.

Create a large DynamicSurface.

Run:

```text
continuous SnakeHook / Grab / Clay strokes
```

using brush-relative topology while a Utility workload runs in parallel.

Measure:

```text
dab P50
dab P95
dab P99

remesh P95
BVH update P95
normal update P95

utility throughput

topology validity
determinism
```

Test should assert:

```text
no corruption
no deadlock
no unbounded latency growth
no starvation
```

---

# 73. Long-session test

Run thousands of adaptive strokes.

Example deterministic workload:

```text
for 5000 strokes:

    choose brush from:
      Clay
      Crease
      Grab
      SnakeHook
      Smooth
      Flatten
      Pinch

    vary:
      radius
      detail_resolution
      pressure

    every 100:
      validate topology

    every 250:
      serialize / reload

    every 400:
      undo / redo

    periodically:
      trigger Utility BVH rebuild
```

Record latency over time:

```text
stroke 100
stroke 1000
stroke 2500
stroke 5000
```

Performance must not degrade simply because:

```text
slot pools
free lists
history
BVH leaves
scratch capacity
```

have evolved.

---

# 74. Proposed implementation phases

## Phase A — Current Dyntopo audit

Create an engineering matrix:

```text
DynamicSurface
slot pools/generations
attribute domains
constraints
split
collapse
flip
local remesh
DynamicBvh
DynamicSculptor
TopologyDelta
serialization
C ABI
pyclay
examples
tests
```

Mark each:

```text
DONE
PARTIAL
MISSING
UNTESTED
```

Do not rewrite components already passing the intended contract.

## Phase B — Dyntopo correctness hardening

Focus on:

```text
operator refusal tests
constraint propagation
determinism
random torture
undo identity restoration
serialization
```

## Phase C — Dyntopo workflow completion

Add/verify:

```text
whole-stroke path
one stroke = one history step
dirty chunk host transport
binding parity
```

## Phase D — WorkClass foundation

Implement:

```text
WorkClass
per-job class
platform policy abstraction
backward-compatible for_range overload
```

No behavior changes to call sites yet except internal default.

## Phase E — Apple QoS

Implement per-job worker QoS mapping.

Test worker QoS and ensure restore behavior.

## Phase F — classify call sites

First:

```text
interactive brush paths
```

Then:

```text
UserInitiated long operations
Utility maintenance
Background work
```

## Phase G — iPad tuning

Tune:

```text
worker count
chunks per worker
min_chunk
```

against P95/P99 latency.

## Phase H — combined stress gates

Gate release on:

```text
Dyntopo under Utility contention
long-session stability
thermal duration
```

---

# 75. Suggested task list — Dynamic Topology

```text
D1  Audit `DynamicSurface` invariants.
D2  Audit generation restoration on undo/deserialize.
D3  Add exhaustive split tests.
D4  Add exhaustive collapse/link-condition tests.
D5  Add exhaustive flip tests.
D6  Test Boundary constraint behavior.
D7  Test UV seam propagation.
D8  Test Sharp edge propagation.
D9  Test Material/UserLocked refusal.
D10 Add stationary-brush hysteresis convergence test.
D11 Verify BrushRelative detail scaling.
D12 Verify `max_ops_per_stamp` latency bound.
D13 Verify remesh timing for every adaptive brush verb.
D14 Verify shared automask/falloff/kernel parity.
D15 Add full resolved-stroke integration if absent.
D16 Guarantee one stroke = one `TopologyDelta`.
D17 Add randomized topology torture.
D18 Add deterministic replay corpus.
D19 Verify local normal updates.
D20 Verify DynamicBvh brute-force query oracles.
D21 Verify update_many touches only affected leaves/ancestors.
D22 Complete dirty-chunk C ABI.
D23 Complete pyclay parity.
D24 Add Swift smoke if C ABI grows.
D25 Add numbered Dyntopo example that renders and asserts.
D26 Run release/ASan/UBSan/binding/release gates.
```

---

# 76. Suggested task list — Mobile Threads / QoS

```text
Q1  Add `parallel::WorkClass`.
Q2  Add per-job work class to ThreadPool.
Q3  Add backward-compatible `for_range` overload.
Q4  Add platform thread-policy abstraction.
Q5  Implement Apple QoS mapping.
Q6  Apply/restore class per worker job generation.
Q7  Apply class to caller participation scope.
Q8  Unit-test nested-call behavior after QoS changes.
Q9  Unit-test condition-variable join/deadlock behavior.
Q10 Audit every `parallel::for_range` call site.
Q11 Mark sculpt critical paths Interactive.
Q12 Mark explicit long operations UserInitiated.
Q13 Mark rebuild/cache maintenance Utility.
Q14 Mark genuine non-urgent work Background.
Q15 Add startup worker-limit configuration if device data requires it.
Q16 Benchmark worker-count matrix on reference iPads.
Q17 Benchmark chunks-per-worker matrix.
Q18 Benchmark idle Interactive P50/P95/P99.
Q19 Benchmark Interactive under Utility contention.
Q20 Add sustained 15–30 minute thermal test.
Q21 Add combined Dyntopo + QoS stress test.
Q22 Update C ABI only for global configuration, not per-operation QoS.
Q23 Update docs/host threading contract.
Q24 Add release gate for iPad latency regression.
```

---

# 77. OpenSpec proposal structure

Recommended:

```text
openspec/changes/add-dynamic-topology/
    proposal.md
    design.md
    tasks.md
    specs/

openspec/changes/add-mobile-thread-scheduling/
    proposal.md
    design.md
    tasks.md
    specs/
```

If `add-dynamic-topology` already exists as an active/archived change in the repository history, update/complete the existing contract rather than creating a duplicate.

Suggested Mobile Scheduling requirements:

```text
WorkClassification
AppleQoSMapping
InteractivePriority
NestedParallelSafety
SleepingJoin
ConfigurableWorkerLimit
HeterogeneousCoreCompatibility
SynchronousCallSemantics
CancellationCompatibility
InteractiveLatencyGate
SustainedDeviceGate
```

Suggested Dyntopo requirements:

```text
StableTopologyIdentity
AttributeDomains
ConstraintAwareSplit
ConstraintAwareCollapse
ConstraintAwareFlip
LocalAdaptiveRemesh
BrushRelativeDetail
RemeshHysteresis
BoundedPerDabWork
DynamicSpatialIndex
SparseTopologyUndo
LocalNormalUpdate
DeterministicTopology
HostDirtyTransport
BindingParity
```

---

# 78. Definition of done — Dyntopo

Dyntopo is complete only when:

```text
[ ] DynamicSurface remains separate from fixed Mesh.
[ ] Stable generational IDs survive all topology operations.
[ ] Split is atomic and attribute-aware.
[ ] Collapse uses a valid link condition.
[ ] Collapse cannot create degenerate/inverted/non-manifold topology.
[ ] Flip is deterministic and improves quality.
[ ] Constraints are enforced by operators.
[ ] Local remeshing never scans/remeshes the whole surface.
[ ] BrushRelative detail changes edge scale with brush radius.
[ ] Split/collapse hysteresis converges.
[ ] Remesh work has a strict per-dab budget.
[ ] Remesh timing is correct per brush.
[ ] DynamicSculptor uses shared brush math.
[ ] Full strokes reach DynamicSurface.
[ ] One stroke is one history step.
[ ] TopologyDelta restores identity as well as geometry.
[ ] Normals update locally.
[ ] DynamicBvh maintains edits incrementally.
[ ] Full BVH rebuild never occurs mid-drag.
[ ] Dirty-chunk host transport exists.
[ ] Deterministic replay passes.
[ ] Random topology torture passes under sanitizers.
[ ] C ABI + pyclay parity passes.
[ ] Swift smoke is extended if ABI grows.
[ ] Numbered example renders and asserts behavior.
```

---

# 79. Definition of done — Mobile Threads / QoS

Mobile scheduling is complete only when:

```text
[ ] Work is classified as Interactive/UserInitiated/Utility/Background.
[ ] Existing synchronous semantics remain clear.
[ ] Old `for_range` callers remain source-compatible.
[ ] Worker jobs carry a WorkClass.
[ ] Apple workers execute with mapped QoS.
[ ] QoS is changed/restored safely across persistent worker jobs.
[ ] Nested calls still execute safely inline.
[ ] Final join sleeps rather than spins.
[ ] Cancellation never bypasses job completion bookkeeping.
[ ] Interactive sculpt call sites are explicitly classified.
[ ] Utility/background call sites do not run as Interactive by accident.
[ ] Worker count is measured on reference iPads.
[ ] Chunk granularity is measured on reference iPads.
[ ] Interactive P50/P95/P99 is recorded.
[ ] Utility contention regression is gated.
[ ] Sustained thermal behavior is measured.
[ ] Dyntopo remains responsive under supported concurrent workload.
[ ] No deadlock/starvation occurs in stress tests.
[ ] Documentation explains host threading responsibilities.
```

---

# 80. Final target architecture

```text
                    Pencil / Mouse
                          │
                          ▼
                    Stroke Engine
                          │
                          ▼
                    Dab / Stamp
                          │
              WorkClass::Interactive
                          │
                          ▼
                 DynamicSculptor
                          │
                ┌─────────┴─────────┐
                │                   │
                ▼                   ▼
          DynamicBvh query    local remesher
                │                   │
                └─────────┬─────────┘
                          ▼
                 Shared Brush Runtime
                          │
                          ▼
                   geometry changes
                          │
                 ┌────────┴─────────┐
                 ▼                  ▼
          local normals       DynamicBvh
                              incremental
                 │                  │
                 └────────┬─────────┘
                          ▼
                    TopologyDelta
                          │
                          ▼
                    dirty chunks
                          │
                          ▼
                  partial host update
```

Meanwhile:

```text
DynamicBvh rebuild
cache maintenance
background preparation
```

run at:

```text
Utility / Background
```

and must not destroy interactive latency.

---

# 81. Most important engineering decisions

The team should carry these decisions through implementation:

1. **Do not rewrite Dyntopo.** The core representation/operators/remesher/BVH/sculptor already exist.

2. **Keep topology mutation separate from fixed Mesh.** Do not weaken the fixed-topology contract.

3. **Keep remeshing local and bounded.** Dyntopo should never become a hidden full-surface operation inside a dab.

4. **Stable IDs are part of correctness, not an optimization.**

5. **Do not duplicate brush behavior.** DynamicSurface must continue using the shared brush runtime.

6. **One stroke is one transaction.** Dabs are implementation units, not undo units.

7. **Do not globally recompute normals or rebuild the BVH in an interactive dab.**

8. **Do not make every ClayCore worker permanently UserInteractive.** Classify work.

9. **Use Apple QoS before attempting manual P/E-core affinity.**

10. **Keep the thread-pool design simple until measurement proves a multi-job scheduler is necessary.**

11. **Interactive latency is the QoS acceptance criterion, not merely calling the correct Apple API.**

12. **Test the two features together.** A Dyntopo implementation that is fast in isolation but stalls under utility work is not ready for ClaySpace on iPad.

---

# 82. Recommended immediate development sequence

```text
1. Audit the existing Dyntopo source against the checklist.
2. Close missing correctness/binding/history/dirty-transport items.
3. Add deterministic Dyntopo torture tests.
4. Add WorkClass to the current thread pool.
5. Add Apple QoS mapping.
6. Explicitly classify sculpting paths as Interactive.
7. Explicitly classify maintenance/long operations.
8. Benchmark worker count and chunk size on reference iPads.
9. Run Dyntopo while Utility work competes.
10. Freeze device performance gates only after measurement.
```

This sequence minimizes architectural churn and targets the real remaining risks: **adaptive topology correctness at the host boundary and sustained interactive latency on iPad.**
