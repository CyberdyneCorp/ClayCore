# Tasks: add-mesh-layers

## 1. Nothing to do for item 1 of the issue

- [ ] 1.1 Confirm and record that `clay_mesh_load` with an import budget already
      exists for OBJ, PLY and FBX, matches the extension case-insensitively,
      treats a null budget as the defaults and a zeroed field as that field's
      default, and refuses an over-budget file before allocating. Specified,
      tested and released at 0.22.0 — the issue is stale on this point and no
      code changes here.
- [ ] 1.2 Record the one real import gap, glTF/GLB being write-only, as a
      follow-up rather than folding it into this change.

## 2. The document model

- [ ] 2.1 `scene::LayerKind::Mesh`, appended so the existing enumerators keep
      their values and the layer record's kind byte keeps its layout
- [ ] 2.2 `std::map<scene::LayerId, mesh::Mesh>` on `io::ClaySpaceDoc`, beside
      `voxel_layers` and `masks`. Nothing in `clay::scene` includes
      `clay/mesh/mesh_data.h`; `tools/check_layering.py` is left unamended and
      is the proof
- [ ] 2.3 Mesh layers are created and removed through `AddLayerCmd` /
      `RemoveLayerCmd`, not by mutating `kind` behind the vocabulary's back the
      way `clay_document_add_voxel_layer` does — so both are undoable and both
      serialize
- [ ] 2.4 Removing a mesh layer does NOT erase the payload; the save and load
      filtering in section 3 is what keeps an orphan harmless
- [ ] 2.5 `instance_layer` keeps refusing a non-SDF layer, mesh included
- [ ] 2.6 Tests: evaluation and every compiled tape bit-identical with and
      without a mesh layer; add/remove undo; instancing refused

## 3. Persistence

- [ ] 3.1 A mesh codec in `clay::io` beside the formats it already owns —
      `u32 vertex_count`, `u32 index_count`, `u8 attribute_mask`, arrays
      little-endian, `u32` indices, uncompressed. Not a member of `mesh::Mesh`,
      which is a plain interchange struct; the divergence from
      `VoxelGrid::serialize` is deliberate
- [ ] 3.2 A `MESH` chunk per mesh layer, payload `u32 layer_id` plus the stream,
      written after `VOXL`/`MASK` and decoded in the same chain
- [ ] 3.3 Guardrails BEFORE allocating: vertex count bounded by what the
      remaining bytes could hold given the declared attributes, index count
      bounded by the remaining bytes and a multiple of three, every index below
      the vertex count. The index bound is the one that matters — a host reads
      these buffers by borrowed pointer
- [ ] 3.4 Write a chunk only for a layer id that exists as a mesh layer; drop on
      load any chunk whose layer id names no mesh layer
- [ ] 3.5 `kClaySpaceMinor` and `scene::kSceneMinor` 4 → 5 together; the static
      assertion binding them stays. Major unchanged
- [ ] 3.6 Format notes record what an older reader loses on re-save, the way
      earlier minors record their own losses
- [ ] 3.7 Tests: the golden-corpus round trip extended with mesh layers — same
      arrays, and saving again produces identical bytes; a document written
      before this change loads with no mesh layers; a document with mesh layers
      opens under the previous minor's reader with its other layers intact
- [ ] 3.8 Tests: over-declared vertex count, out-of-range index, index count not
      a multiple of three, an orphaned payload not written, an unmatched chunk
      dropped

## 4. The C ABI

- [ ] 4.1 `CLAY_LAYER_KIND_MESH`, with the compile-time assertion tying it to
      the engine enumerator that every other enumeration carries
- [ ] 4.2 Attach an already-loaded mesh as a layer: versioned descriptor with
      the mandatory leading `struct_size`, its own attach budget, an optional
      uniform import scale, returning the layer id and a borrowed mesh handle
- [ ] 4.3 Look up a mesh layer; a name no mesh layer carries returns not-found
- [ ] 4.4 Mesh bounds off the mesh handle. `clay_layer_bounds` is left alone —
      `pick` may not see mesh data and would report an empty box
- [ ] 4.5 `clay_mesh` gains a hidden owned/borrowed discriminator; destroy is a
      no-op on a borrowed handle, since the call returns no status and changing
      its signature would break every consumer for a case that cannot arise
      today. Documented beside the existing lifetime note
- [ ] 4.6 Transform and concatenate as primitives; one convenience call meshing
      the field and appending every visible mesh layer under its transform
- [ ] 4.7 The merge rebases indices and DROPS an attribute that is present on
      some inputs and absent on others, rather than returning a short array.
      Stated in the header
- [ ] 4.8 `clay_document_mesh` is untouched; a regression test holds it
      bit-identical on a document that now has mesh layers
- [ ] 4.9 `max_file_bytes` appended to `clay_import_budget`, and a new
      budget-taking document load entry point beside the existing one — a new
      call, not a changed arity, which is what 0.22.0 taught
- [ ] 4.10 `tools/check_c_abi.py` clean: descriptors carry `struct_size`, every
      declared symbol resolves, counts are bounded
- [ ] 4.11 Tests: attach and reload through C; undo of an attach; the attach
      budget refusing an oversized mesh; not-found; destroy on a borrowed handle
      leaving the document readable; the combined export; the attribute drop;
      a hidden layer excluded while ghost and lock change nothing

## 5. Python

- [ ] 5.1 `pyclay` gains attach, lookup, listing, bounds and the combined
      export, with the mesh buffers as numpy views over engine memory
- [ ] 5.2 `tools/check_binding_parity.py` clean — the gate is one-way, so a
      C-only surface would pass silently and is not an excuse
- [ ] 5.3 Parity test: attach, transform, export through both surfaces produces
      identical meshes

## 6. Documentation and release

- [ ] 6.1 An `examples/` script: import a model, attach it, sculpt beside it,
      export both together
- [ ] 6.2 `docs/RELEASE.md`: ABI 0.24.0 → 0.25.0, additive — no existing
      signature changes and no existing descriptor field moves — plus the
      `.clayspace` 1.4 → 1.5 note and what an older reader does
- [ ] 6.3 Format documentation for the `MESH` chunk and the mesh stream
- [ ] 6.4 `openspec/ROADMAP.md` reconciled

## 7. Deliberately not in this change

Recorded here so the boundary is visible rather than assumed.

- [ ] 7.1 No SDF participation: no tape, no blend, no influence bound, no brick
      culling. `clay_item_volume_from_mesh` stays the only route from triangles
      to field and is untouched
- [ ] 7.2 No picking or raycasting against imported triangles — `pick` gates on
      SDF and may not include `mesh`; enabling it needs the picking spec and the
      layering table amended
- [ ] 7.3 No contribution to `clay_layer_bounds`
- [ ] 7.4 No non-uniform or matrix layer transform
- [ ] 7.5 No glTF/GLB import, no materials, textures, second UV set or
      multi-material meshes
- [ ] 7.6 No N meshes per layer, no mesh-layer instancing, no per-layer meshing
      of SDF layers, no mesh editing verbs as commands, no streaming or LOD, no
      chunk compression
- [ ] 7.7 The voxel and mask chunks keep their own orphan behaviour; the mesh
      rule makes the inconsistency visible and a follow-up can close it
- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): runs in parallel with expose-scene-groups, add-consolidation-policy and add-multi-resolution; touches no VoxelGrid code
- [ ] 0.2 This change takes `.clayspace` minor **5**. The minors are assigned in the
      roadmap rather than taken first-come, because three open changes each add a chunk and
      two bumping independently yields a document claiming one minor while carrying one
      feature. Bump `kClaySpaceMinor` and `kSceneMinor` together — a static_assert binds them

