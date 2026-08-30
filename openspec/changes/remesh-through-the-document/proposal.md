# Proposal: remesh through the document

## Why

`add-voxel-remesher` shipped the geometry and said, in its own design notes,
what it deliberately did not ship:

> The pure geometry operation ships first and alone. It has no document, no
> layer, no history and no revision token, which is what makes it testable and
> reusable.

That was the right order and it is not a finished feature. An artist's remesh
has to land on a LAYER, replace what was there, and be one step on the undo
menu. Today a host holds both meshes and commits the pair itself — which means
every host reimplements the same four decisions, and gets the same four things
wrong.

Three of them are not obvious, and were found by building this:

- **A stale commit silently wins.** A host that runs the rebuild on a worker
  thread and commits when it finishes will overwrite whatever the artist did
  while waiting. Nothing in the library could tell it not to.
- **A live sculptor survives a rebuild that should have killed it.** The two
  checks that existed — the layer's mesh POINTER and the sculptor's vertex and
  index COUNTS — both pass a replacement that happens to land on the same
  counts, leaving an adjacency and a BVH describing triangles that no longer
  exist. Every stamp after it moves the wrong vertices, silently.
- **Vertex deltas already on the undo stack outlive their mesh.** They were
  recorded against the old vertex count, and `History::apply_step` applies them
  to whatever the resolver now returns.

## What changes

- **`session::Step::Kind::MeshReplace`** — one layer's whole mesh, before and
  after, as ONE undo step, with the journal entry that lets it survive a crash
  and the `step_bytes` term that lets the budget see it.
- **A mesh layer geometry revision** — bumped by a wholesale replacement and
  never by a sculpt, because a sculpt is exactly the change a cached adjacency,
  BVH or sculptor SURVIVES.
- **`clay_document_voxel_remesh_layer`** — capture, rebuild, validate, replace,
  record: one call, one undo step, transactional.
- **`clay_document_replace_mesh_layer`** with an expected revision, for a host
  that ran the pure operation on a worker thread and now wants to commit it.
- **`clay_document_mesh_layer_revision`** to read the token.
- The same three on `pyclay.Document`, and the revision check inside the
  Python sculptor.
- **Report fields the guide asked for and V1 omitted**: the result's one-sided
  distance to the source (RMS, p95, max), per-stage wall clock, and the memory
  figure the resource guard actually compared against the budget.

## Approach

The pure operation is untouched. `clay_document_voxel_remesh_layer` is exactly
`clay_mesh_voxel_remesh` followed by `clay_document_replace_mesh_layer` at the
layer's own revision, and both go through one internal replacement helper — so
there is a single place that holds the guards, the undo record and the
invalidation, and no way for the one-call form and the two-call form to drift.

The revision is a counter on the document handle rather than on `mesh::Mesh`,
because it describes a LAYER's history and not a surface. An owned mesh belongs
to no layer, cannot be replaced under anyone, and carries no revision.

## Non-goals

- Async execution inside the library. The revision is what makes a host's own
  worker thread safe; the library stays synchronous and says so.
- A general "replace any layer payload" history kind. This one is for meshes,
  because a mesh is the only payload whose whole-snapshot cost is justified by
  how rarely it is replaced.
- Repairing `DynamicSurface::from_mesh`'s refusal of marched meshes, which this
  change documents and does not fix — see the design note.

## Impact

`scene-model` gains the undo-scope and stale-result requirements. `c-abi` and
`python-bindings` gain the surface. `meshing` gains the report fields and the
corrected projection rule. Nothing existing changes behaviour except the
projection weight, which is a measured improvement described in the design.
