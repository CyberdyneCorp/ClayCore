## ADDED Requirements

### Requirement: Topology-derived preparation is shared, not rebuilt per sculptor

Building a sculptor over a mesh SHALL NOT repeat the topology-derived
preparation another live sculptor over the same unchanged mesh has already
paid for. On a ~296k-triangle mesh that preparation is the whole of a
sculptor's construction cost — 120 ms of weld-and-CSR against a sculptor whose
remaining members are empty — and a host that opens a second session on one
layer, or recreates a sculptor between strokes, pays it again for a partition
that did not change.

The shared object SHALL be owned by the document or by an explicit cache the
caller holds, and SHALL NOT be a process-global map: a document close SHALL
release it, two documents SHALL NOT collide on one identity, and a test SHALL
be able to run in isolation.

A cache entry SHALL be keyed on a stable identity and SHALL NOT be keyed on
vertex or index counts, which different connectivity can share.

#### Scenario: A second sculptor over unchanged topology does not rebuild
- **WHEN** a second sculptor is created over a mesh whose connectivity has not changed since the first
- **THEN** it reuses the first's topology data and does not repeat the adjacency construction

#### Scenario: A wholesale replacement is not reused
- **WHEN** a mesh layer's triangles are replaced
- **THEN** a sculptor created afterwards builds topology data over the new triangles

#### Scenario: Two documents do not collide
- **WHEN** two documents each hold a mesh layer with the same identity
- **THEN** neither is served the other's topology data

### Requirement: A shared topology entry is validated rather than trusted

A cache hit SHALL be verified against the mesh it is about to be served for,
and a verification failure SHALL be a miss that rebuilds. Verification SHALL
cover at least the vertex count, the triangle count, the construction option
that changes weld semantics, and the connectivity itself — so that two meshes
with identical vertex and index counts and different connectivity cannot be
served each other's data.

**The reason is that the counter such a cache would otherwise be keyed on has
been observed not to move.** A cache whose correctness is the correctness of a
revision inherits every defect in every path that should have bumped it.
Verification costs a fraction of the construction it decides to skip, and buys
correctness that does not depend on invalidation hygiene elsewhere.

Explicit invalidation SHALL still exist and SHALL still be called on wholesale
replacement. The two are not alternatives: one is the intentional, cheap path
and the other is the net under it.

#### Scenario: Identical counts, different connectivity
- **WHEN** a lookup is made for a mesh with the same vertex and triangle counts as a cached entry but different connectivity, and nothing invalidated the entry
- **THEN** the lookup misses and topology data is built over the mesh actually presented

#### Scenario: Positions moved by sculpting keep the entry
- **WHEN** a stroke has moved vertices without changing connectivity, and a new sculptor is created
- **THEN** the cached topology data is served, which is what a sculptor live across that same stroke would have held

### Requirement: The topology cache is visible and trimmable

The cache SHALL report its occupancy — entries, bytes, hits, misses, evictions,
and the time spent building and verifying — and its bytes SHALL appear in the
document's memory report inside the rebuildable roll-up rather than being
invisible memory a host cannot account for.

A trim SHALL release every entry no live sculptor is holding, and SHALL release
nothing a live sculptor is holding. Both halves are required: a cache that
cannot be released under pressure is a leak with a good reason, and one that
releases storage a sculptor still refers to is a crash.

#### Scenario: A trim releases what nothing holds
- **WHEN** a trim runs while no sculptor holds a cached entry
- **THEN** the entry is released and the bytes released are reported

#### Scenario: A trim leaves a live entry alone
- **WHEN** a trim runs while a sculptor holds a cached entry
- **THEN** that entry is retained and the sculptor continues to work
