# Proposal: serialize without a file

## Why

Every format this library supports has a **byte-buffer form in C++ and only a
path form across the ABI**. From `include/clay/io/`:

| in the engine | across the C ABI |
|---|---|
| `save_clayspace(doc) -> vector<uint8_t>` / `load_clayspace(data, size, out)` | `clay_document_save(doc, path)` / `clay_document_load(path, out)` |
| `save_obj -> string` / `load_obj(text, out)` | — |
| `save_ply -> vector<uint8_t>` / `load_ply(data, size, out)` | — |
| `save_fbx -> vector<uint8_t>` / `load_fbx(data, size, out)` | — |
| `save_glb -> vector<uint8_t>` / `load_glb(data, size, out)` | — |
| | `clay_mesh_save(mesh, path)` / `clay_mesh_load(path, budget, out)` |

The file forms are thin wrappers over the byte forms — `save_obj_file` is
`write_whole_file(path, save_obj(...))`. The boundary exposes the wrapper and
hides the thing it wraps. Same shape as `report-mesh-quality`: the engine has
it, the boundary discards it.

**This is the host seam, and it is where the library is being pointed.** A
sculpting app on iPadOS does not receive documents as stable paths. It receives
them from `UIDocument`, `NSFileProvider` and the share sheet, behind
security-scoped URLs whose access is bracketed and whose lifetime it does not
control. A desktop host syncing to a server needs bytes to `PUT`. A host storing
projects in its own container — a package, a database, a zip — needs bytes. A
WASM build has no filesystem at all. Every one of those must currently round-trip
through a temporary file, which means:

- a second full copy of the document on a device that is short of both space and
  memory,
- a path that has to be created, permissioned, cleaned up, and cleaned up
  *again* after a crash,
- and a failure mode — a full disk — that has nothing to do with what the host
  asked for.

**It also blocks the change that comes after it.** Crash recovery wants to
append serialized commands to a journal, which means bytes, not a path. Nothing
in this repository can produce those bytes through the ABI today.

## What changes

An owner-handle for bytes, in the shape `clay_mesh` and `clay_tape` already
established — serialize once, borrow the bytes, release — rather than the
size-query pattern, which would serialize the document twice to answer the
size and then fill the buffer.

- **`clay_blob`**, opaque, with `clay_blob_data` / `clay_blob_size` borrowing
  and `clay_blob_destroy` freeing. One type for every serialized payload, so a
  host learns it once.
- **`clay_document_save_memory` / `clay_document_load_memory`.**
- **`clay_mesh_save_memory` / `clay_mesh_load_memory`**, which take the format
  by NAME rather than by extension, because a buffer has no extension. The
  names are the extensions without the dot, matched case-insensitively, so a
  host that already has `"obj"` from a path does not have to translate.
- **The mesh import budget applies to the memory loaders unchanged.** A buffer
  from the network is the untrusted input the guardrails exist for, and it
  would be precisely backwards for the in-memory path to be the unguarded one.
  Its file-byte ceiling is the one exception and is *not* offered: it bounds
  the bytes a loader reads into memory before sizing a buffer, and a caller
  holding a buffer has already done that read — `load_clayspace` takes no
  budget for exactly this reason, and `load_clayspace_file` uses the budget for
  nothing else. So `clay_document_load_memory` takes none, and the header says
  why. A parameter that cannot act is worse than an absent one.
- **pyclay** gets `Document.to_bytes()` / `clay.load_bytes()` and
  `Mesh.to_bytes(format)` / `clay.load_mesh_bytes(...)`, so the parity gate
  stays clean and a test can round-trip without touching disk.

## What this is NOT

**Not streaming, and not incremental.** One call in, one buffer out. A document
too large to serialize into memory at once is a real problem and this is not
its answer; chunked or resumable serialization would own progress, lifetime and
partial state, which is a different change. What this removes is the temporary
file, not the peak.

**Not a new format, and not a format change.** The bytes are byte-identical to
what the file forms already write — that is testable, and it is the test.

**Not a change to the file entry points.** They keep working and keep their
behaviour; they become what they already were in the engine, wrappers over the
memory forms.

**Not the OBJ sidecar.** `save_obj_file` writes a companion `.mtl` and names it
in the OBJ's `mtllib` line. A buffer has no companion, so the in-memory OBJ
carries **no `mtllib` line at all** rather than a reference to a file that does
not exist. Stated here because a dangling reference is the plausible bug and
silence is the correct behaviour.
