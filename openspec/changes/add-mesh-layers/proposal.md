# Proposal: a mesh a document carries rather than samples

## Why

An artist imports a model for one of two reasons. Either they want to sculpt on
it — and `add-mesh-to-field-import` closed that: a mesh becomes a
`field::FieldVolume`, an item carrying a volume, an operand like any other. Or
they want to *keep* it: a scan, a scale reference, a kit part, a piece of hard
surface authored elsewhere that must leave the pipeline as the same triangles it
entered with. Nothing in the engine does the second.

Sampling is not carrying, and the archived proposal said so in as many words:
before it landed, an imported mesh was "something you can display and export,
not something you can work on". That change made it workable. It did not make it
carryable — a volume is a resampled approximation on a lattice frozen at bake
time, with uvs and normals discarded, and once baked the triangles are gone. A
reference model round-tripped through `clay_item_volume_from_mesh` comes back as
a different mesh, at a resolution someone picked, and a document that contains
one cannot re-export what was imported.

So the missing object is the opposite of the one we built: triangles stored
verbatim, never compiled into a tape, never blended, present for display and for
re-export.

## Item 1 of the issue is already done

The issue this proposal answers asks for two things, and the first of them
exists. `clay_mesh_load` takes a path and a `clay_import_budget` and returns a
mesh handle (`bindings/c/clay.h:665`), covering OBJ, PLY and FBX with the
extension matched case-insensitively; a null budget means the defaults, a zeroed
field means that field's default, and an over-budget file is refused before
anything is allocated. It is specified (`c-abi` — "The import budget is settable
across the ABI", "A file extension is matched case-insensitively"), tested
(`tests/unit/test_c_volume.cpp`), mirrored in `pyclay.load_mesh`, and its arity
change is already in `docs/RELEASE.md` under 0.22.0. This proposal asks for no
work there and does not re-propose it.

The one genuine import gap is that glTF/GLB is write-only — `save_glb` exists,
no `load_glb` symbol does — and that is neither what the issue asks for nor what
this change is about. It is recorded as a follow-up.

## What a mesh layer is, and what it is not

A third `scene::LayerKind`, `Mesh`, holding one imported mesh, with the mesh
itself stored *beside* the document keyed by layer id — where voxel grids and
mask fields already live — rather than on the `Layer`.

That placement is not a detail. `tools/check_layering.py` gives `clay::scene`
the dependencies `{kernel, math, field}` and deliberately withholds `mesh`, so
the payload physically cannot enter the evaluated document. The mask requirement
already states the principle this borrows: keeping non-evaluated content out of
`scene::Document` makes "this does not change what the document evaluates to"
structural rather than a property somebody has to maintain. A mesh layer is the
strongest case for that discipline, because every follow-up anyone will ask for
— pick it, snap to it, boolean against it — pulls toward the field.

The layer level is also where the skip already exists and is already load
bearing. Tape build, picking, the C ABI's layer walk and the test utilities all
gate on `kind == Sdf` and ignore anything else; voxel proved that a non-SDF kind
costs zero field semantics. The alternative — a display-only *item* inside an
SDF layer — would force every consumer of the edit list to learn a case that
means "not actually an edit".

Visibility, ghost, lock, ordering and the transform are the `Layer`'s existing
fields, edited through the existing commands. Nothing new is invented for a mesh
layer that a layer did not already have.

## What Changes

- **`scene::LayerKind::Mesh`**, one mesh per mesh layer, created through
  `AddLayerCmd` so it is undoable and serializes like every other layer. The
  payload is a `mesh::Mesh` on `io::ClaySpaceDoc` keyed by `LayerId`, beside
  `voxel_layers` and `masks`.
- **A `MESH` chunk in `.clayspace`**, one per mesh layer, and a mesh codec in
  `clay::io` beside the formats it already owns. The container stores the
  decoded triangles, not a path: a reference outside the file would make the
  round-trip requirement — reloads bit-identically, reserializes to identical
  bytes — depend on a file we do not control. The source path is kept as
  advisory provenance and never resolved.
- **`kClaySpaceMinor` and `scene::kSceneMinor` move 4 → 5** together, because
  the layer record's `kind` byte gains a meaning. The major stays 1: an older
  reader skips the unknown chunk and ignores a layer whose kind it does not
  recognise, and — as with every earlier minor — loses the mesh if it re-saves.
- **The C ABI gains the mesh-layer surface**: attach an already-loaded mesh to a
  document as a layer, look one up as a borrowed handle, ask a mesh for its
  bounds, transform a mesh, concatenate meshes. Additive; 0.24.0 → 0.25.0.
- **`clay_import_budget` gains `max_file_bytes`** and document loading gains a
  budget-taking entry point beside the existing one. `load_clayspace_file` takes
  a budget today and the C boundary cannot pass one, which was academic while
  documents held only tapes and sparse grids. Embedded meshes make the default
  ceiling real, and `file-io` already requires that a document's read ceiling be
  the caller's to raise.
- **Export merges explicitly.** `clay_document_mesh` is untouched: it prices a
  dense grid from the tape's own bounds, and folding in triangles that are not
  in the tape would either inflate that grid or leave the mesh outside it, and
  would change what an existing call returns for an existing document. Instead
  the merge primitives are exposed and a convenience call combines the meshed
  field with every visible mesh layer, transformed, with the attribute-drop rule
  stated rather than silent.
- **`pyclay` gets the same surface** in the same change, because the binding
  parity gate is one-way and would not otherwise notice.

## What this change does not do

- **No SDF participation, ever.** A mesh layer does not enter a tape, does not
  blend, does not affect `clay_eval`, bricks or culling.
  `clay_item_volume_from_mesh` remains the only route from triangles to field
  and is untouched. This is the line the spec holds in the mask requirement's
  own words: its presence does not change what the document evaluates to.
- **No picking or raycasting against imported triangles.** `pick` gates on
  `Sdf` and may not include `mesh` under the layering table; `mesh::Bvh` makes
  it easy later, and picking has its own requirements a later change would have
  to amend.
- **No contribution to `clay_layer_bounds`**, which is derived from SDF shapes.
  Framing an imported model is answered by bounds on the mesh handle instead.
- **No non-uniform scale.** `math::Transform` is position, rotation and a
  uniform scale by design. Unit and axis conversion happen at import, baked into
  the vertices, as the FBX loader already does.
- **No glTF/GLB import, no materials, no texture references, no second UV set,
  no multi-material meshes, no compression, no N meshes per layer, no instancing
  of mesh layers, no per-layer meshing of SDF layers, no mesh editing verbs as
  document commands.** Each is listed in `design.md` with what it would cost.

## Capabilities

### Modified Capabilities

- `scene-model`: a document's layers may be `mesh` kind; what such a layer
  carries, and that it changes nothing about evaluation.
- `file-io`: the mesh chunk, its guardrails, the minor bump, and what an older
  reader does with it.
- `c-abi`: the mesh-layer surface, borrowed-handle lifetime, the export merge,
  and a file ceiling a C caller can raise.
- `python-bindings`: the same surface from `pyclay`, for parity.

## Impact

- `include/clay/scene/document.h` (a third `LayerKind`),
  `include/clay/io/clayspace.h` and `src/io/clayspace.cpp` (the payload map and
  the chunk), a new mesh codec in `clay::io`, `src/scene/commands.cpp` (the
  minor), `bindings/c/clay.h` + `clay_c.cpp`, `bindings/python/`, tests, an
  example, `docs/RELEASE.md`.
- Format: `.clayspace` 1.4 → 1.5, backward-open, no forward refusal.
- ABI 0.24.0 → 0.25.0, additive: no existing signature changes and no existing
  descriptor field moves.
- Three gates fire on this change — `check_layering.py`, `check_c_abi.py`,
  `check_binding_parity.py` — and each is a task rather than an afterthought.
