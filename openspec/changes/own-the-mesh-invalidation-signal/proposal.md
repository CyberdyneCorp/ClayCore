# Proposal: own the mesh invalidation signal

## Why

`clay_document_mesh_layer_revision` exists for exactly one moment — a layer's
triangles being replaced wholesale — and it did not move at that moment when
history was the thing doing the replacing. Measured by a host on v0.84.0 and
re-verified here:

| step     | triangles | revision |
| -------- | --------- | -------- |
| attached | 119,100   | 1        |
| rebuilt  | 37,752    | 2        |
| undo     | 119,100   | 2        |
| redo     | 37,752    | 2        |

Undo restored every vertex and every index and left the token where it was. The
consequence is a REFUSED STROKE on the next dab, with nothing naming the cause:
the host is sculpting through an adjacency and a BVH built over triangles that
are no longer in the document. Their workaround is to record the engine undo
depth each rebuild sits at and reconcile it themselves — a host reimplementing
the library's own invalidation because the library's does not fire (#472).

The cause is structural rather than a missed call site. The counter lived on the
C ABI's `clay_document` handle, and it had exactly two writers. Undo, redo and
journal replay restore a mesh through `session::History`'s `mesh::Mesh*`
resolver, which hands out a pointer and has no way to tell a binding's separate
map which layers were replaced. pyclay kept a second map with the same two
writers and the same gap.

## What changes

- **The generation moves to `io::ClaySpaceDoc`, beside the triangles it counts**,
  with `install_mesh_geometry` as the only way triangles enter a layer. Attach,
  rebuild, load, undo, redo and journal replay all land there, and the install
  cannot happen without the advance. It is RUNTIME-ONLY: not serialized, no
  format minor, and a reopened document starts a fresh generation domain.
- **`session::History::MeshInstaller`** — a set-once seam, distinct from
  `MeshFor`, used by the `MeshReplace` step and the `MeshReplace` journal event.
  A vertex-delta step still writes through the pointer, because a delta does not
  change the mesh's identity and is exactly what the caches survive.
- **pyclay's parallel map is deleted**, not synchronised. Both bindings now read
  the document's own counter, so the two cannot drift.
- The observable rule, stated: undo does not RESTORE the historical number, it
  ADVANCES. rebuild = 2, undo = 3, redo = 4.

## Approach

The engineering guide's rule — the mutation should own its invalidation signal —
picks the shape. The alternative was to have undo and redo report which mesh
layers they replaced and bump in the C ABI; it is rejected in the design,
because it leaves journal replay, recovery and every future wholesale-replacement
command to remember a second call, which is the failure that produced this bug.

## Non-goals

- Serializing the revision. A saved document cannot carry a token for caches
  that do not survive the save; see the design.
- Bumping on anything but a wholesale replacement. A sculpt, a rename, a
  visibility toggle, a transform and history on another layer are each asserted
  NOT to move it — a fix that bumps on everything would cost a host the rebuild
  it is trying to avoid.

## Impact

`scene-model` gains the advancing-generation and refused-replacement rules.
`c-abi` and `python-bindings` restate what their entry point promises. No ABI
surface changes, no format minor, no behaviour changes for anything but the
paths that were silent.
