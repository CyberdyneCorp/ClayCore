# Tasks: own the mesh invalidation signal

## 1. The generation moves beside the triangles

- [x] 1.1 `mesh_geometry_revision` on `io::ClaySpaceDoc`, runtime-only, with
      the comment saying why it is not serialized
- [x] 1.2 `install_mesh_geometry` — the one place triangles enter a layer, and
      it advances as it installs
- [x] 1.3 `mesh_revision` reader, defaulting to 1 for a layer holding none
- [x] 1.4 `note_mesh_geometry_replaced` for the one in-place wholesale rewrite
      (`clay_mesh_weld` on a borrowed layer)
- [x] 1.5 `read_mesh_chunk` installs through it, and `drop_unmatched_mesh_chunks`
      drops the generation with the triangles it counted

## 2. History reaches the owner

- [x] 2.1 `session::History::MeshInstaller`, set once, distinct from `MeshFor`
- [x] 2.2 `History::install_mesh` — the one funnel, used by the `MeshReplace`
      step and by the `MeshReplace` journal event
- [x] 2.3 `Step::Kind::Mesh` still writes through `MeshFor`: a vertex delta is
      exactly what the caches survive

## 3. The bindings share the one mechanism

- [x] 3.1 C ABI: the map leaves `clay_document`, `mesh_layer_revision_of` reads
      the document's, `clay_document_add_mesh_layer` and
      `replace_mesh_layer_geometry` install through it
- [x] 3.2 `clay_document_enable_undo` sets the installer
- [x] 3.3 pyclay's parallel map is DELETED; `py_replace_mesh_layer`,
      `Document.mesh_layer_revision`, `Document.add_mesh_layer` and
      `PyMeshSculptor` all read the document's
- [x] 3.4 pyclay `Document.enable_undo` sets the installer

## 4. Tests

- [x] 4.1 `tests/unit/test_c_mesh_layer_revision.cpp`: attach, rebuild, undo,
      redo — the revision strictly advances AND the restored triangles match the
      expected snapshot at every transition
- [x] 4.2 Repeated undo/redo cycles: every transition a new generation
- [x] 4.3 A SECOND path — journal replay onto a document holding the same layer —
      which fails if the bump sits in the public undo entry point
- [x] 4.4 The negatives: a sculpt dab, a rename, a visibility toggle, a
      transform-only edit, history on another layer, a refused replacement, an
      undo with nothing to undo
- [x] 4.5 The same three cases in `bindings/python/tests/test_pyclay.py`, so the
      two bindings are held to one answer
- [x] 4.6 Proven by revert: the installer removed, the suite fails on the
      numbers the issue reported

## 5. Documentation

- [x] 5.1 `clay_document_mesh_layer_revision`'s header comment says undo,
      redo and replay advance it, that it advances rather than restores, and
      that it is per-session
- [x] 5.2 `Document.mesh_layer_revision`'s docstring says the same
- [x] 5.3 The ROADMAP entry for #472 records the outcome
- [x] 5.4 Two consequences the CONSUMER found, neither of which is a stale
      cache, both now on the header and one of them gated:
      - **A held token across a reopen fails by AGREEING.** A fresh domain
        starts at 1, so a stored 1 read against a fresh 1 says "unchanged" for a
        different mesh. The header already said a token does not survive a
        reopen; it did not say the failure is agreement rather than a mismatch
        anyone would notice. Invisible to a host whose open path builds a new
        document and assigns over the old one — which is what theirs does, and
        why they could not have hit it — and reachable by one that reuses a
        layer table
      - **`replace_mesh_layer`'s expected-revision CAS now refuses a commit it
        used to take.** Read a revision, let the artist undo and redo back to
        the SAME triangles, then commit: the number moved twice while the
        content came back, so the commit is refused with
        `CLAY_ERROR_FORWARD_VERSION` where through 0.84.0 it succeeded. Correct
        under this change's own rule — a token names the generation a result was
        computed against, and that generation is gone even though the vertices
        agree — but a refusal a host did not previously have to handle. Gated by
        "a round trip through undo and redo refuses a commit it used to take",
        which asserts BOTH halves: that the content really did come back
        identical, and that the commit is still refused. A refusal on changed
        content would prove nothing. It also asserts the remedy works, because a
        refusal a host cannot clear would be worse than the bug this change
        fixes
