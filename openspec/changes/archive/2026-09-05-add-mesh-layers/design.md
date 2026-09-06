# Design: mesh layers

## Context

`clay::scene` models a document as an ordered list of layers, each `Sdf` or
`Voxel` kind. An SDF layer holds an edit list that compiles into a tape; a voxel
layer holds nothing in the document at all — its grid lives on
`io::ClaySpaceDoc` keyed by layer id, and so does the optional mask field. Every
consumer of the layer list gates on `kind == Sdf` and silently skips the rest:
tape build, picking, the C ABI's layer walk, the test utilities.

Triangles reach the engine two ways today. `clay_mesh_load` decodes OBJ, PLY and
FBX into a `mesh::Mesh` under an import budget, and `clay_item_volume_from_mesh`
samples one into a `field::FieldVolume` so it can be sculpted. There is no third
way, and no place a document can hold triangles as triangles.

`tools/check_layering.py` grants `clay::scene` the dependencies
`{kernel, math, field}`. It does not grant `mesh`. `clay::io` may see both.

## Goals / Non-Goals

**Goals:**

- A document can carry an imported mesh, in a layer, with a transform,
  visibility, ghost, lock and ordering, and can save and reload it byte
  identically.
- The mesh is reachable as contiguous buffers a headless host can render.
- The mesh can be re-exported, alone or merged with the meshed field.
- Nothing about evaluation changes, structurally rather than by convention.
- Every existing C signature and descriptor field keeps working unchanged.

**Non-Goals:**

- Any participation in the SDF: no tape, no blending, no culling, no bricks.
- Picking, snapping or raycasting against imported triangles.
- Contribution to `clay_layer_bounds`.
- Non-uniform or matrix layer transforms.
- glTF/GLB import; materials, textures, multiple UV sets, multi-material meshes.
- Mesh editing verbs as document commands, mesh-layer instancing, N meshes per
  layer, per-layer meshing of SDF layers, streaming or LOD, chunk compression.
- Anything in item 1 of the originating issue: `clay_mesh_load` with a budget
  already exists, is specified, tested and released.

## Decisions

- **D1 — A layer kind, not an item, and not a list of mesh objects.** `Layer`
  already carries id, name, kind, transform, visible, ghost, locked, and
  ordering by position, which is exactly the property set an imported mesh
  needs and no more. The node level is defined by SDF semantics — prim, op,
  blend, rounding, deformers, all of it there to be compiled — so a
  "display-only item" would teach every edit-list consumer a case meaning "not
  actually an edit". The layer level already has the skip, it is already load
  bearing, and voxel proved a non-SDF kind costs zero field semantics.
  Consequence accepted: one mesh per mesh layer, a mesh layer holds no SDF
  items, and `Document::instance_layer` keeps its refusal of a non-SDF layer.
  The chunk is shaped so N-per-layer stays additive (D5).

- **D2 — The payload lives beside the document, not on the `Layer`.** Three
  independent reasons, any one sufficient. The layering gate forbids
  `clay::scene` from including `clay/mesh/mesh_data.h`, and amending the table
  is exactly the containment we want to keep. `AddLayerCmd` carries a whole
  `Layer` by value and the undo stack records inverses, so a mesh on the layer
  puts multi-megabyte copies on the undo stack. And the mask requirement
  already states the principle: content the document does not evaluate stays
  out of the document, which makes the no-change-to-evaluation claim structural.

- **D3 — Store decoded triangles, not a path.** `file-io` requires a reloaded
  document to evaluate bit-identically and reserialize to identical bytes. A
  path reference makes those bytes depend on a file outside the container and
  on the importer's version: the same `.clayspace` yields different geometry
  after the OBJ is edited and fails to open when it is gone. "Meshes are large"
  is already answered the same way for sampled volumes and voxel grids — the
  reader takes a budget precisely so a document this library wrote stays
  openable. The engine is also headless and driven from CI, where the
  filesystem the import came from may not exist. The source path is stored as
  advisory provenance, verbatim and never resolved.

- **D4 — Stored untouched.** The document holds what the importer returned: no
  welding, reordering, renormalizing or reindexing on the way in. Byte-identical
  round trip is then trivially satisfied because the arrays are stored rather
  than re-derived — contrast `VoxelGrid::serialize`, which has to sort chunks to
  be deterministic. Any future "tidy up on import" breaks the golden-corpus
  round trip, which is the point of writing this down.

- **D5 — One new `MESH` chunk, payload `u32 layer_id` plus a mesh stream.** The
  stream is `u32 vertex_count`, `u32 index_count`, `u8 attribute_mask` for
  normals / colors / uvs, then the float arrays little-endian, then `u32`
  indices. Uncompressed: triangle data does not run-length encode usefully and a
  general compressor would be a new dependency in a codebase whose readers are
  deliberately dependency-free. Repeating the chunk for one layer id is the
  cheap forward path to N-per-layer if it is ever wanted.

- **D6 — The codec lives in `clay::io`, not as a member of `mesh::Mesh`.**
  `mesh::Mesh` is a plain interchange struct with no invariants of its own, and
  `io` already owns every byte format that touches it. This diverges from
  `VoxelGrid::serialize` and `MaskField::serialize`, which are members; the
  divergence is deliberate and cheap to reverse if consistency is preferred.

- **D7 — Both minors move 4 → 5, the major stays 1.** A new chunk alone would
  not require it, since unknown chunks are skipped, but the layer record's
  `kind` byte gains a value and that is the same class of change that moved the
  minor for the ghost/lock packing. `kClaySpaceMinor` and `scene::kSceneMinor`
  are bound by a static assertion, so they move together. Forward refusal stays
  reserved for a real break: an older reader skips the `MESH` chunk, reads the
  unknown kind into the enum's `uint8_t` underlying type, ignores the layer at
  every `kind == Sdf` gate, and dereferences nothing because there is no SDF
  content to dereference. It loses the mesh if it re-saves, which is stated in
  the format notes the way earlier minors state their own losses.

- **D8 — A chunk and its layer must match, both ways.** Write a `MESH` chunk
  only for layer ids that exist as mesh-kind layers; drop on load any chunk
  whose layer id has no mesh layer. That one rule makes an orphaned map entry
  harmless and makes undo of a layer removal work within a session — the inverse
  of `RemoveLayerCmd` restores a `Layer` by value and cannot carry the payload,
  so the payload must simply never be erased on removal. Voxel grids and masks
  have the same hole today; fixing them is out of scope, but the mesh rule makes
  the inconsistency visible and it is worth a line in the follow-ups.

- **D9 — The transform is `Layer::xform`, applied by the consumer.** It already
  serializes inside the layer record, is already undoable through
  `SetLayerTransformCmd`, is already protected by the lock flag and is already
  reachable from both bindings. Vertices are stored in the space the importer
  produced and the transform is applied at read or export time — which is the
  rule voxel content already lives under implicitly, and this change states it.
  `math::Transform` carries a uniform scale only, so unit and axis conversion
  are baked into the vertices at import, as the FBX loader already does for
  metres; an optional uniform import scale is exposed on the attach path. A
  determinant-negative transform is not expressible and is not attempted.

- **D10 — Attach takes an already-loaded mesh, not a path.** `clay_mesh_load`
  exists and already owns import policy including the budget, so a path-taking
  attach would duplicate it. Two-step also composes with
  `clay_mesh_from_triangles`, so a host that generated geometry itself attaches
  it without touching the filesystem.

- **D11 — A borrowed mesh handle is a silent no-op to destroy.** Voxel grids and
  masks already carry an owned/borrowed discriminator, and
  `clay_voxel_grid_destroy` *reports* the refusal. `clay_mesh_destroy` returns
  `void`, so it cannot. Giving `clay_mesh` the same hidden discriminator and
  making destroy a no-op on a borrowed handle is safe precisely because only
  handles that could not previously exist are affected; changing the signature
  would be an ABI break for a case that cannot arise today. The header states
  it beside the existing lifetime note.

- **D12 — Bounds come off the mesh handle, not off `clay_layer_bounds`.**
  `pick::layer_bounds` returns empty without SDF content, and `pick` may not
  include `mesh` under the layering table. Computing it where both modules are
  visible is the honest placement, and it keeps `clay_layer_bounds` meaning what
  it has always meant.

- **D13 — `clay_document_mesh` is untouched; merging is explicit.** That call
  compiles the tape, refuses an empty document, derives its region from the
  tape's bounds and prices a dense grid before allocating. Folding triangles in
  would make the sampled region depend on geometry that is not in the tape,
  change what an existing call returns for an existing document, and push
  attribute policy into a call whose contract is "mesh the field". Voxel layers
  are already outside it for the same reason. So: `clay_mesh_transform` and
  `clay_mesh_concat` as primitives, plus one convenience call that meshes the
  field and appends every visible mesh layer. Concatenation rebases indices; an
  attribute present on some inputs and absent on others is dropped from the
  result, because a loader may not return a mesh whose normals, colors or uvs
  are non-empty and a different length than its positions; `visible == false`
  excludes a mesh layer, while `ghost` and `locked` do not change what is
  exported, consistent with neither flag changing what a document evaluates to.

- **D14 — `clay_import_budget` gains `max_file_bytes`, and document loading
  gains a budget-taking entry point beside the existing one.**
  `load_clayspace_file` takes a budget; `clay_document_load` passes the default
  and offers no way to override it, and the descriptor has no file-bytes field
  at all. That was academic while documents held tapes and sparse grids; with
  embedded meshes it is not, and `file-io` already requires that the read
  ceiling be the caller's to raise. Appending a field to a versioned descriptor
  is additive by construction. A *new* entry point rather than a changed arity:
  the 0.22.0 note in `docs/RELEASE.md` is the cautionary precedent.

## Risks / Trade-offs

- **Documents get large.** Nothing caps what the writer writes, and
  `save_clayspace` builds the whole byte vector in memory before writing it, so
  peak memory on save is roughly twice the document. D14 is the mitigation on
  the read side; the write side is unchanged and stated rather than fixed.
- **The import defaults are enormous for something that goes into a document.**
  50M vertices and 100M triangles is 600 MB of positions alone. Attaching takes
  its own, tighter, caller-visible budget rather than inheriting the loader's.
- **Scope creep toward "a mesh is just another operand".** Every follow-up —
  pick it, snap to it, boolean against it, retopologize it — pulls toward the
  field. Containment is the explicit no-change-to-evaluation requirement plus
  the layering gate, which physically prevents `scene` from seeing mesh data.
- **Version coupling.** `kClaySpaceMinor == scene::kSceneMinor` is statically
  asserted. Forgetting that the kind byte changed meaning and leaving the minor
  at 4 makes older builds mis-decode rather than skip, which is the one way this
  change could corrupt a document.
- **Payload and document can drift.** The id-keyed map can desynchronize from
  the layer list, as voxel already can. D8 is the containment and it is tested
  both directions: an orphan is not written, an unmatched chunk is dropped.
- **A borrowed mesh handle outliving its document** is a use-after-free the
  no-op destroy does not prevent. It gets the doc comment and a test, the same
  way the borrowed voxel handle does.
- **Silent attribute loss on export.** A painted sculpt merged with an unpainted
  import loses colors across the whole result. The rule is documented and the
  drop is stated at the call, not discovered afterwards.
- **Byte-identical round trip holds only while D4 holds.** Any later
  normalization on import breaks the golden-corpus test, which is why D4 is a
  decision and not an implementation note.

## Open questions

These are product calls, not code facts. The first cut assumes the answer given.

1. **One mesh per layer, or a kit of parts in one layer?** Assumed one (D1); the
   chunk shape keeps the other cheap (D5).
2. **Is `.glb` import in this change?** Assumed no. It is the only real import
   gap and it is not what the issue asks for.
3. **Compress the mesh chunk?** Assumed no (D5). If file size is a product
   constraint, that changes.
4. **Embed the original file bytes instead of decoded triangles?** It would
   preserve materials and UV sets the importer drops. Rejected on round-trip
   determinism (D3), but it is a fidelity-versus-determinism call.
5. **Should a mesh layer be pickable or frameable in the first cut?** Assumed
   no; enabling it needs the picking spec and the layering table amended.
6. **Store the source path at all?** Assumed yes, verbatim and advisory (D3);
   an absolute path is a portability and privacy leak.
7. **Target ABI version and release timing.** 0.25.0 assumed; confirm against
   whatever else is queued.
8. **Fix the voxel and mask orphan-chunk behaviour at the same time?** Assumed
   no (D8), but the mesh rule makes the inconsistency visible.
