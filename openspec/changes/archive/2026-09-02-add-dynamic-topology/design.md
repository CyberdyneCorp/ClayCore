# Design: dynamic topology

## Context

`mesh::Mesh` is flat, contiguous and shared by every producer and consumer in
the library. `mesh::Adjacency` builds CSR over weld classes once and is reused
while positions move; its `matches()` checks vertex and index counts, and its
header says it goes stale when either changes. `mesh::MeshSculptor` holds
adjacency, an optional BVH and per-stamp scratch across a stroke because they
are expensive to build and cheap to keep. `mesh::Bvh::refit` updates the bounds
of named triangles and refuses a topology change. `mesh::VertexDeltas` records
positions, normals and colours and deliberately records no indices, because the
contract says indices cannot change. `session::History` reverses all of it
through resolvers passed in from above, since `check_layering.py` forbids
`scene` from seeing `mesh` at all.

Every one of those is correct for fixed topology and hostile to mutable
topology. That is the whole design problem: the pieces are not wrong, they are
specialized, and the specialization is load-bearing.

## Goals / Non-Goals

**Goals:**

- A surface whose connectivity changes locally without renumbering anything
  else, and without invalidating handles a caller holds.
- Split, collapse and flip that are individually atomic and refuse what would
  corrupt the surface.
- Brush-driven local remeshing whose cost follows the brush footprint.
- One gesture is one sparse undo step, including topology.
- Round trip to `mesh::Mesh` preserving geometry and the attributes the
  representation supports.

**Non-Goals:**

- Any change to `MeshSculptor`, `Adjacency`, `Bvh` or `VertexDeltas`
  semantics.
- Quads. A dynamic surface is triangles; `quads` is provenance on import and
  is cleared on export.
- Multiresolution. A hierarchy over a mutable base is the next change and is
  deliberately a different representation.
- GPU topology mutation.
- Non-manifold input as a supported authoring state.

## Decisions

- **D1 — A half-edge surface in slot pools, not a mutated `Mesh`.** The naive
  implementation edits `positions` and `indices` in place, and every one of its
  costs is structural: an erase is O(mesh), a reallocation moves memory a host
  may be mid-upload with, the BVH's triangle numbering shifts under it, and
  adjacency must be rebuilt. Slot pools with a free list and a generation
  counter per slot make a local edit local, and make a stale handle detectable
  rather than silently valid — which matters most exactly where it is hardest
  to debug, at a slot that was freed and reused.

- **D2 — Chunked leaves, not a per-face mutable BVH.** Both work. Chunks of a
  few hundred triangles win because the chunk is simultaneously the BVH leaf,
  the brush candidate set, the parallel work unit, the normal-recompute unit,
  the dirty-tracking unit and the host's upload unit. A per-face tree gives one
  of those and leaves the library to invent a different granularity for each of
  the others. The brick cache already demonstrates the shape: a sparse set of
  fixed units with revisions, refilled and uploaded independently.

- **D3 — Corner-domain UVs from the first commit, even though P0 does not
  author them.** A seam is represented today by position-coincident duplicate
  vertices, which a mutable surface must either weld — destroying the UVs — or
  treat as disconnected geometry, which cracks under remeshing. The
  representation has to be able to say "one geometric vertex, two UVs" before
  any operator runs, because retrofitting an attribute domain means rewriting
  every operator that interpolates.

- **D4 — Constraints are edge flags, not policy in the remesher.** Boundary,
  UV seam, sharp, material and user-locked ride on the edge, so an operator
  refuses on its own rather than depending on a caller to have filtered its
  input. An operator that is safe only when called correctly is a bug waiting
  for the second caller.

- **D5 — Determinism is an ordering rule, not a consequence.** The mesh verbs
  promise bit-identical results on every run and every platform, and the voxel
  falloff dither is hashed on a cell coordinate specifically to keep that
  promise across backends. A remesher that iterates a hash map produces a
  different sequence of splits on a different standard library and fails that
  promise silently, with a plausible-looking surface. So the candidate set is
  sorted by stable id before any operator runs, and the requirement says so.

- **D6 — Undo records local reversible deltas, never a snapshot.** A
  multi-million-triangle snapshot per stroke is not an undo system, it is a
  memory leak with a keyboard shortcut. The delta records created and deleted
  elements, connectivity changes and attribute changes, coalesced over the
  gesture the way `VertexDeltas` already coalesces positions — first `before`,
  last `after`, one entry per element.

- **D7 — The C ABI ships dirty chunks, and the whole-mesh export stays for
  correctness.** A host that copies a full mesh per dab cannot use this at the
  sizes it exists for. But a whole-mesh path that is obviously correct is what
  the dirty path is tested against, so both exist and the test compares them.

- **D8 — Revisions are triple, not single.** Topology, geometry and attributes
  advance independently, and a host that must re-upload an index buffer only
  when connectivity changed needs to be told which happened. The same
  distinction serves cache invalidation for anything derived.

## Risks / Trade-offs

- **The representation count.** This makes five authoritative representations,
  and every promise the library makes in the singular — one stroke engine, one
  mask model, one document, one undo history — is a cross-product that grows
  with it. Mitigated by sharing the brush kernels and the mask gate rather than
  by hoping.
- **Memory per element.** A half-edge surface costs several times a flat mesh
  per triangle. Measured explicitly per element class rather than estimated,
  and paged so deleted slots are reused.
- **BVH quality drift.** Local leaf updates keep the tree correct and let its
  quality decay; the fixed BVH already documents that refit stays correct and
  does not stay fast. A quality metric marks a leaf for rebuild and the rebuild
  happens between strokes, not mid-drag.
- **A conversion that loses quads.** Anything that enters a dynamic surface
  leaves as triangles. That is the price and it is stated at the boundary,
  because a quad export is the format the retopology pipeline downstream
  requires.

## Migration Plan

Additive throughout. `mesh::Mesh`, `MeshSculptor`, `Adjacency`, `Bvh` and
`VertexDeltas` are untouched. The document format gains a chunk that older
readers skip; a document with no dynamic surface is byte-identical to one
written before the change. The C ABI grows opaque handles and entry points and
changes no existing signature.

## Open Questions

- Whether a mesh layer OWNS a dynamic surface or a host does — engine types
  first, document integration in a separate change, is the recommendation, and
  the file-io delta here assumes the eventual owner rather than requiring it
  now.
- Whether collapse placement ever needs the quadric the offline decimator
  already computes.
- Whether the remesher's relax pass is the existing `Relax` kernel applied to a
  mutable surface or a tangential smoothing of its own.

---

## Decisions taken at implementation (section 1 of the tasks)

### D9 — The HOST owns a dynamic surface; the document does not, yet

Engine types now, document ownership as its own change. `DynamicSurface` is
constructed from a `mesh::Mesh`, sculpted, and exported back at explicit
boundaries; nothing in `scene` or `io` learns about it in this change.

The reason is not caution for its own sake. Owning one from a mesh layer means
a new `LayerKind` payload, a `.clayspace` chunk and a format minor, and the
format decision that comes with it — whether a saved dynamic surface stores
its half-edge structure or re-imports from triangles on load — is a decision
about a representation nobody has sculpted with yet. The serialization in
section 8 exists so that decision has a format to reach for when it is taken;
it is not wired into the document here.

### D10 — Determinism is an ordering rule over stable ids

Already D5 in principle; this states the mechanism the code uses. Every place
that gathers candidate edges or faces for a topology operation SORTS them by
their stable slot index before running a single operator. Not by pointer, not
by hash-map order, not by the order a spatial query returned them — the query's
order depends on the tree's shape, and the tree's shape depends on the history
of edits.

The fixed sculptor already learned this in the small: `MeshSculptor::gather`
sorts the euclidean region's class list precisely so that a stamp does not
depend on whether the host happened to have built a BVH. The same reasoning at
a larger scale is that a remesher's SEQUENCE of operations is observable in the
final connectivity, so the sequence has to be a function of the input alone.

### D11 — A dynamic surface is triangles, and export re-derives no quads

`to_mesh` writes `quads` empty and the documentation says so at the boundary.

Re-deriving quads would be a pairing heuristic — two triangles sharing an edge
whose union is convex enough — and a heuristic is exactly the wrong thing here:
the quad export is what the retopology pipeline downstream consumes, and a
pipeline fed heuristic quads that vary with the sculpt is worse off than one
told plainly that this representation is triangles. A caller who wants quads
retopologises, which is what `add-sculpt-handoff-export` already serves.

### D12 — Collapse places at the MIDPOINT, and constraints win over geometry

For P0:

- both endpoints unconstrained → the midpoint;
- exactly one endpoint constrained (boundary, seam, sharp, user-locked) → the
  constrained endpoint's position, so the feature does not move;
- both endpoints constrained → **refused**, unless the edge itself carries the
  same constraint, in which case it collapses ALONG the feature to the
  midpoint and the feature keeps its shape.

The quadric the offline decimator computes is not used. It answers "where does
this vertex go to minimise error against the ORIGINAL surface", which is the
right question for a one-shot simplification and the wrong one under a brush:
the original surface is the thing being deliberately changed, and a placement
that resists that is a placement that fights the artist. The open question
stays open for a future decimation-quality pass over a finished sculpt.
