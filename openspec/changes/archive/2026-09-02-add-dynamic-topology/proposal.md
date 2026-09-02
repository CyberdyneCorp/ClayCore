# Proposal: dynamic topology

## Why

A mesh layer can be sculpted and cannot grow. Sixteen verbs move the vertices
an import supplied, and when a snakehook pulls a tendril out of a coarse sphere
the triangles stretch until the surface is unusable. The library documents that
as the artist's signal to leave and retopologise elsewhere, and for a year that
was the honest boundary: the SDF side sidesteps topology entirely, so competing
on dynamic tessellation was not this engine's fight.

Shipping fixed-topology brushes is what made the boundary a problem rather than
a scope. An engine that invites an artist to sculpt a mesh and then tells them
the result is a signal to go somewhere else has spent the workflow it opened.
The audit of the outside review recorded the half-agreement already — a local
remesh is *a recovery operation, not a sculpting mode* — and the decision taken
now is that the distinction does not survive contact with the implementation:
the same local split, collapse and flip that repairs a stretch is the whole of
dynamic topology minus the policy that drives it. Building the repair and
refusing the policy would be building the expensive half.

What is actually missing is narrower than "dyntopo" and worth naming precisely:
**there is no representation in this library whose connectivity can change.**
`Mesh` is flat arrays a mutation renumbers. `Adjacency` is CSR that goes stale
on a count change and says so. `Bvh::refit` refuses a topology change by
design, and is right to. `VertexDeltas` deliberately records no indices. Every
one of those is a correct decision for the representation it serves, and
together they mean adaptive topology cannot be retrofitted — it has to be a
representation of its own.

## What changes

- **`mesh::DynamicSurface`** — a triangular half-edge surface in slot pools
  with generation-tagged handles, so a local edit renumbers nothing and a stale
  handle is detected rather than silently rebound. Imports from and exports to
  `mesh::Mesh`.
- **Local topology operators** — split, collapse and flip, each atomic, each
  refusing the cases that corrupt a surface (inversion, non-manifold edges,
  duplicated triangles, boundary closure, seam destruction).
- **Edge constraints** — boundary, UV seam, sharp, material, user-locked — so
  the operators know what they may not touch.
- **A chunked mutable spatial index** whose leaves are the unit of topology
  mutation, spatial query, parallel work, dirty tracking and host upload.
- **A local remesher** driven by a brush-relative target edge length with
  split/collapse hysteresis, and a per-verb remesh timing policy.
- **`mesh::DynamicSculptor`** — the shared brush kernels over the mutable
  surface, with geodesic gather over stable ids and mask support.
- **`mesh::TopologyDelta`** and a history kind for it, so one gesture of
  hundreds of topology operations is one sparse, reversible undo step.
- **A dirty-chunk C ABI**, because exporting a whole mesh per dab is fatal at
  the sizes this exists to serve.

## Approach

Beside `MeshSculptor`, never inside it. The fixed-topology contract — no verb
creates, splits, deletes or reorders a polygon; `indices` and `quads` come out
byte-identical — is what makes a mesh layer worth holding after a retopology
pass, and it stays exactly as it is. Adaptive topology is a representation a
caller converts into deliberately.

`mesh::Mesh` remains the interchange format and gains nothing. Every existing
producer and consumer — meshers, exporters, validation, decimation, the C
accessors — keeps working on the same flat arrays, and a dynamic surface
crosses to them at explicit boundaries.

Attribute domains are separated from the start: UVs are corner data, colour and
mask are vertex data. A split that averages UVs across a seam destroys it
silently, and a representation that cannot express the distinction cannot be
retrofitted with it later.

## Open questions

- **What owns a dynamic surface.** The cheap answer is the host, exporting to
  `mesh::Mesh` at boundaries and leaving the document format alone. The honest
  answer is a mesh layer that can hold one, which is a `.clayspace` chunk and a
  layer-kind decision.
- **Whether collapse placement needs quadric error.** Midpoint is adequate for
  brush work; the offline decimator already carries the quadric machinery.
- **How determinism is stated.** The mesh verbs promise bit-identical results
  on every run and every platform. A remesher whose operation order comes from
  hash-map iteration cannot keep that promise, and the requirement has to say
  what the order is rather than assume single-threading supplies one.
- **Whether quads survive a conversion at all.** They cannot survive
  remeshing; the question is whether the export re-derives any, or states
  plainly that a dynamic surface is triangles and a quad workflow does not pass
  through it.

## Impact

A new `dynamic-topology` capability. `meshing` gains a scoped contract — the
fixed-topology guarantee belongs to `MeshSculptor` specifically, and the
"SHALL NOT re-tessellate" sentence is narrowed to the fixed-topology layer
rather than to the library. `scene-model` gains a history kind and memory
rows; `file-io` gains a versioned format; `c-abi` and `python-bindings` gain
the surface. Nothing existing changes behaviour: a document that never creates
a dynamic surface is bit-identical to one built before this change.
