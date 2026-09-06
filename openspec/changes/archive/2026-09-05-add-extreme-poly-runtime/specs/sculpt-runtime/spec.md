# sculpt-runtime

Delta for `add-extreme-poly-runtime`. A new capability: what a sculpt costs at
scale, and what a memory-constrained host may release.

## ADDED Requirements

### Requirement: One chunk unit serves every subsystem
The surface runtime SHALL partition a surface into chunks that serve simultaneously as the spatial index leaf, the brush candidate set, the parallel work unit, the normal recomputation unit, the dirty-tracking unit and the host's upload unit.

Inventing a different granularity per subsystem is the failure this requirement exists to prevent: each one is defensible alone, and together they mean a change tracked at one granularity has to be translated into another before anything can act on it.

A chunk SHALL carry separate revisions for topology, geometry, normals and attributes, so a consumer re-derives only what actually changed.

The chunk size SHALL be chosen from a measurement over this library's own workloads rather than adopted from prior art, and the measurement SHALL cover query cost, false-positive touched vertices, normal recomputation, upload size and mutation cost.

#### Scenario: A stamp dirties the chunks it touched
- **WHEN** a stamp changes one region of a large surface
- **THEN** the dirty set names the chunks covering that region and no others

#### Scenario: Geometry moves without an index re-upload
- **WHEN** a stamp moves vertices without changing connectivity
- **THEN** the geometry revision advances and the topology revision does not

### Requirement: An ordinary dab does no work proportional to the model
For a fixed brush footprint, no ordinary stamp SHALL scan, allocate, copy, rebuild or upload storage proportional to the total surface.

Specifically: no whole-surface adjacency build, no whole spatial index rebuild, no whole-surface copy for preview, no whole-level multires reconstruction, and no evaluation of a layer stack over geometry the stamp did not reach.

Stamp cost SHALL remain within one band as the surface grows across at least an order of magnitude, allowing for logarithmic traversal and cache effects.

After warm-up, an ordinary stamp on a stable-topology surface SHALL perform no heap allocation. An adaptive surface SHALL allocate only from pools it preallocated.

Spatial index quality SHALL be tracked, and a rebuild SHALL be deferred to between strokes rather than performed mid-drag. A local refit keeps a tree correct and does not keep it fast, and rebuilding is not automatically an improvement — the fixed-topology index already records that measurement.

Maintenance that is not required for correctness — index rebuilds, cache compaction, storage promotion, pool compaction — SHALL be queued for a host to service with a time budget between interactions, and SHALL NOT run inside a pointer event.

#### Scenario: A twentyfold model is not a twentyfold stamp
- **WHEN** the same footprint is stamped on surfaces of 1M and 20M vertices
- **THEN** the stamp times stay within one band rather than scaling with the vertex count

#### Scenario: Warm stamps do not allocate
- **WHEN** a stroke of similar footprints runs after the first stamp
- **THEN** the instrumented allocation count for the subsequent stamps is zero

### Requirement: A host declares its memory budget and the library honours it
The library SHALL accept a memory profile — a memory class and explicit byte budgets for runtime caches, undo, scratch, preview and resident levels — supplied BY THE HOST.

The portable core SHALL NOT detect a device, call a platform API or branch on a device model. A host knows what its operating system is telling it; an engine guessing from a model name is both wrong and untestable.

The profile SHALL be exercisable on a desktop, so that constrained behaviour is tested where the tests run rather than only where it ships.

#### Scenario: Constrained behaviour is testable off-device
- **WHEN** a constrained profile with small budgets is set on a desktop build
- **THEN** the runtime behaves as it would on a constrained device, and the tests observe it

### Requirement: Memory is reported by what a host may do with it
The memory report SHALL separate, for every sculpt representation: authoritative content, undo, and rebuildable caches — chunk indices, per-level runtime caches, evaluated layer caches, derived positions for inactive levels, scratch and preview staging.

It SHALL roll those up into three totals a host can act on directly: essential, rebuildable, undoable.

A single total SHALL NOT be treated as the answer. Under pressure a host does not need to know how big the document is; it needs to know WHICH PART, because that decides what it is allowed to release.

#### Scenario: The report answers the question a memory warning asks
- **WHEN** a document holding an adaptive surface, a hierarchy with several levels and a layer stack is measured
- **THEN** the report gives the authoritative bytes, the rebuildable bytes and the undo bytes separately

### Requirement: Trimming releases caches in a stated order and never the work
The library SHALL release memory on request at a stated pressure level, in this order: transient scratch beyond its steady capacity; preview staging; evaluated caches; spatial indices for inactive levels; derived positions for inactive levels; other rebuildable caches; and history, to the host's own policy.

It SHALL NEVER release unsaved authoritative content: base geometry, topology, multires detail, sculpt layers or their masks.

A trim SHALL report what it released and how much.

After a trim at any pressure level, the authoritative content SHALL be unchanged — verified by checksum, not by inspection — and every released cache SHALL reconstruct to an identical evaluated surface.

A trim SHALL be safe BETWEEN THE DABS OF A STROKE, not only between strokes: a trim is called from an operating-system callback and lands where the host did not schedule it. A stroke interrupted by any number of trims SHALL commit exactly the surface the same stroke commits uninterrupted, and every dab SHALL land — a sculptor holding a reference into released storage is the failure this states, and it reports the classes it moved while changing nothing.

#### Scenario: A trim between two dabs of a stroke costs no dab
- **WHEN** the same stroke is taken twice, once undisturbed and once with a critical trim after every dab
- **THEN** the authoritative checksum advances after every dab of both, and the two surfaces are identical level by level

#### Scenario: A critical trim costs no work
- **WHEN** a critical trim runs on a document with every cache built
- **THEN** the authoritative checksums are unchanged and the reconstructed surface after rebuilding the caches is identical

#### Scenario: A trim reports what it did
- **WHEN** a trim runs
- **THEN** it names the categories released and the bytes released from each

### Requirement: An operation whose peak exceeds its result says so first
Any operation whose transient peak memory exceeds the size of its result — adding a subdivision level, converting between representations, flattening a layer stack, a global remesh, serializing a large surface — SHALL estimate its authoritative, runtime and PEAK cost and SHALL refuse with a typed budget error rather than allocating part of it.

The estimate SHALL use checked arithmetic; an overflow SHALL produce a refusal rather than a small number.

Such operations SHALL be build-then-publish and cancellable, so a refusal, a failure or a cancellation leaves the document exactly as it was.

The peak, not the steady state, is what fails on a memory-constrained device, and an engine that discovers this by being terminated cannot tell the user what happened.

#### Scenario: An over-budget operation refuses before allocating
- **WHEN** an operation whose predicted peak exceeds the declared budget is requested
- **THEN** it returns a typed budget error, allocates nothing, and leaves the document unchanged

#### Scenario: A cancelled conversion leaves the document intact
- **WHEN** a conversion is cancelled part way
- **THEN** the document is byte-identical to before the call

### Requirement: A preview may be approximate; the committed result may not
The runtime MAY, under an interactive budget, defer work that does not change the committed result: exact normals during a drag, spatial index quality, cache maintenance, and the display level a host renders.

It SHALL NOT defer or approximate anything that changes what is committed: the deformation itself, the stroke's trajectory, topology decisions, detail coefficients or layer content.

A memory-saving mode MAY change preview quality, cache residency, display level and maintenance timing. It SHALL NOT change brush strength, remeshing targets, detail coefficients or layer content unless the host explicitly asks for a lower sculpt resolution.

When exact work has been deferred, the final committed state SHALL be exact.

#### Scenario: Deferred normals converge
- **WHEN** a stroke runs with normals deferred and then completes
- **THEN** the final normals equal those produced with per-stamp recomputation

#### Scenario: A memory-saving mode does not change the sculpt
- **WHEN** the same stroke is applied under a full profile and under a memory-saving profile
- **THEN** the committed geometry is identical
