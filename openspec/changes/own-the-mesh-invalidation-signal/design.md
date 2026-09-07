# Design: own the mesh invalidation signal

## The call graph, verified before anything was written

Read out of the tree at `8f8597b4`, not taken from the issue.

**Where the triangles live.** `io::ClaySpaceDoc::mesh_layers`, a
`std::map<scene::LayerId, mesh::Mesh>` (`include/clay/io/clayspace.h`). Both
bindings hold one: `clay_document::doc` in `bindings/c/clay_c.cpp` and
`PyDocument::doc`, a `shared_ptr<io::ClaySpaceDoc>`, in
`bindings/python/pyclay_module.cpp`.

**Who wrote them, before this change.**

| writer | what it did |
| --- | --- |
| `clay_document_add_mesh_layer` | `mesh_layers.insert_or_assign` — no bump |
| `replace_mesh_layer_geometry` (C ABI, static) | assign + bump |
| `clay_mesh_weld` on a borrowed layer | in-place rewrite + bump |
| `read_mesh_chunk` (`src/io/clayspace.cpp`) | `mesh_layers.emplace` — no bump |
| `py_replace_mesh_layer` | assign + bump, into pyclay's own map |
| `Document.add_mesh_layer` (pyclay) | `insert_or_assign` — no bump |
| `History::apply_step`, `Step::Kind::MeshReplace` | `*m = ...` through `MeshFor` — **no bump, and no way to bump** |
| `History::replay`, `JournalEvent::Kind::MeshReplace` | `*m = ...` through `MeshFor` — **same** |

**Who read the counter.** `mesh_layer_revision_of` (C ABI) behind
`clay_document_mesh_layer_revision`, the `expected_revision` guard inside
`replace_mesh_layer_geometry`, `resolve_sculptor`'s staleness check, and
pyclay's `revision_of` behind `Document.mesh_layer_revision` and
`PyMeshSculptor::live`.

**How history reaches a mesh.** `clay_document_undo_bound` →
`session::History::undo(Document&, GridFor, MeshFor, Aabb*, MaskFor)` →
`History::apply_step` → `Step::Kind::MeshReplace`. Redo is the same path with
`forward = true`. `clay_document_replay_journal` →
`History::replay(..., MeshFor, ...)` → the `MeshReplace` event branch, which is
a SECOND assignment site, not a call into the first. That second site is why the
bump cannot go in `clay_document_undo`.

The step's payload is `Step::mesh_before` / `mesh_after`, both whole meshes
(`Step::Kind::MeshReplace` in `include/clay/session/history.h`), recorded by
`History::record_mesh_replace`, which drops a replacement that changed nothing.

**The layering constraint.** `tools/check_layering.py`: `io` may include
`session`; `session` may not include `io`. So `History` cannot name
`ClaySpaceDoc`, and the seam has to be a callable the owner supplies — the shape
`DynamicMeshFor`, `MultiresFor` and `GroupsFor` already use.

## The decision: the generation moves to where the mesh content lives

Two shapes were on the table.

**(a) The mutation owns its invalidation signal.** The generation becomes
`io::ClaySpaceDoc::mesh_geometry_revision`, and `install_mesh_geometry` is the
only way triangles enter a layer:

```cpp
void install_mesh_geometry(scene::LayerId layer, mesh::Mesh triangles) {
    mesh_layers.insert_or_assign(layer, std::move(triangles));
    ++mesh_geometry_revision[layer];
}
```

**(b) Undo and redo report which mesh layers they replaced, and the C ABI bumps.**

**(a) is chosen**, and the reason is the table above rather than a preference.
The eight writers are in four different files across three modules, and the two
that were wrong are the two furthest from the counter. A reporting channel closes
the two paths that are broken TODAY and leaves the shape that broke them intact:
the next wholesale-replacement command, the recovery path, a consolidation
installing a mesh, a future importer that re-attaches into an existing layer
would each have to remember a second call, and forgetting it is silent in exactly
the way this bug was silent. (a) makes the install and the advance one statement
that cannot be half-performed.

(a) also collapses the two bindings' maps into one. pyclay kept its own
`MeshRevisions` with the same two writers and the same gap; both bindings hold a
`ClaySpaceDoc`, so moving the counter there makes "pyclay must not drift from the
C ABI's answer" structurally true rather than a thing to test. The parity test
added by this change is a guard on the entry points, not on two mechanisms.

### What (a) still needed from `session`

`install_mesh_geometry` closes six of the eight writers. It does not reach the
two inside `History`, which write through a `mesh::Mesh*` the owner handed out.
So `History` gains ONE new seam:

```cpp
using MeshInstaller = std::function<bool(scene::LayerId, mesh::Mesh)>;
void set_mesh_installer(MeshInstaller installer);
```

and one private funnel, `History::install_mesh`, that both the `MeshReplace`
step and the `MeshReplace` journal event go through. It is deliberately NOT a
replacement for `MeshFor`: a `Step::Kind::Mesh` vertex delta writes positions
into a mesh whose identity does not change, which is precisely the change a
cached adjacency, BVH or live sculptor survives, and routing it through the
installer would bump on every brush stamp — the failure mode that makes the token
worthless.

Set once rather than passed to `undo`, `redo` and `replay`, following the reason
`DynamicMeshFor` states in the same header: a fourth parameter breaks every host
compiled against it. Left unset, a wholesale restore assigns through `MeshFor`
exactly as it always did, so a caller that keeps no generation is unaffected.
Both bindings set it in the one place each builds a `History` —
`clay_document_enable_undo` and `Document.enable_undo`.

## Serialization: runtime-only, and why

The revision is NOT written to a `.clayspace`, and this change moves no format
minor.

The number is an invalidation token for caches that are LIVE in the current
session — an adjacency, a BVH, a `mesh::MeshSculptor` built over a
`mesh::Mesh&`. None of them survives a process, let alone a save and a reopen, so
there is nothing on the other side of a load for a restored number to invalidate.
A reopened document therefore establishes a fresh generation domain, starting at
1 for every mesh layer the file carries: `read_mesh_chunk` installs through the
same primitive, and `load_clayspace` builds a fresh `ClaySpaceDoc` before moving
it out, so nothing is inherited.

Writing it would also cost something real. `save_clayspace` → `load_clayspace` is
an identity on the payloads today, and a counter in the bytes would make a saved
file depend on how the session got there — two documents with identical geometry
would no longer serialise to identical bytes, which is the property
`snapshot_identity` is built on.

The one case worth stating: a host holding a revision across a save/reopen sees
the number go DOWN. That is correct and is not a hazard — every cache it could
have been holding is gone with the process — and it is why the ABI comment says
the token is meaningful only against the document instance that produced it.

## Undo advances rather than restores

`rebuild = 2, undo = 3, redo = 4`, not `2, 1, 2`.

Restoring the historical value would be the intuitive reading of "undo" and it is
wrong here. The number is an invalidation token for the host's LIVE caches, not
the age of the restored mesh. A host that rebuilt at 1→2, kept a BVH over the
rebuilt triangles, and then undid holds a BVH describing geometry the document no
longer has; a revision handed back to 1 tells it "you are looking at generation 1
again", and the BVH it built at generation 2 is not the one it had at generation
1. Monotonic advance is the only rule under which "my token != the document's
token" means "rebuild", which is the whole contract.

This falls out of the implementation rather than being enforced on top of it:
`++mesh_geometry_revision[layer]` has no other reading.

## What must NOT move it

Asserted, because a fix that bumps on everything is not a fix:

- a vertex-position sculpt dab while the live sculptor stays authoritative,
- a rename, a visibility toggle, a protection change,
- a transform-only edit, and undoing one,
- history affecting ANOTHER mesh layer,
- a REFUSED replacement — stale `expected_revision`, a protected layer, an empty
  or quad-inconsistent replacement,
- an undo or redo with nothing to reverse, which is reported rather than failed
  and must not spend a generation per click.

## Where attach lands

`install_mesh_geometry` on a layer with no entry yet leaves the generation at 1,
because `mesh_geometry_revision[layer]` zero-initialises and is then incremented.
So an attached layer reads 1, which is what the ABI has always documented and what
the host measured. `mesh_revision` still defaults to 1 for a layer holding no
triangles, so no reader has to distinguish "never installed" from "installed
once" — both mean "nothing has been replaced under you".
