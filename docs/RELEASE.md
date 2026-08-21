# Releasing claycore

Versioning follows the `build-packaging` and `c-abi` specs: the C ABI and the
Python API are SemVer; kernel headers may evolve freely within a major; the
`.clayspace` document format version is independent (backward-open,
forward-refuse).

## Before tagging

1. Bump the version in **all three** places — `CMakeLists.txt` `project(VERSION)`,
   `CLAY_ABI_{MAJOR,MINOR,PATCH}` in `bindings/c/clay.h`, and `version` in
   `pyproject.toml`. The checklist's `version` gate fails if they disagree.
2. Run the full checklist locally:

   ```sh
   python3 tools/release_check.py
   ```

   It gates: version agreement, configure/build, the whole ctest suite,
   backend parity for every backend registered in that build, module
   layering, kernel dialect (CPU + CUDA profiles), the license manifest, C
   ABI hygiene + declared-symbol resolution + ctypes FFI, `openspec validate
   --all --strict`, benchmark floors, the **device gate** (see below), and a
   real `pip install .` quickstart in a throwaway venv.
3. On a minor/patch release, read the `clay.h` diff for symbol and
   struct-layout breaks (the ABI gate checks that every declared symbol
   resolves and that the header is bindgen-clean, not history). Below 1.0
   a break is allowed on a minor bump under SemVer's 0.x rule, but it is never
   silent: say so in the release notes, and make the library reject the older
   layout instead of misreading it. **0.2.0 is such a release** — the leading
   `struct_size` shifted every field of `clay_item_desc` and
   `clay_mesh_params`, so ABI 0.1.0 binaries must recompile. **0.22.0 is
   another**: `clay_mesh_load` gained a nullable `const clay_import_budget*`
   between its path and its out-parameter, so a caller compiled against 0.21.0
   gets a compile error rather than a misread — the arity changed, so there is
   no way for old code to link and behave differently.
   **0.35.0 is a third**, and the subtlest of them, because nothing about it
   is visible at compile time: `clay_brick_config_defaults`,
   `clay_stroke_preset_defaults` and `clay_stroke_preset_deserialize` now
   REQUIRE the caller to set `struct_size` before the call, where they used to
   set it themselves. Same signatures, same arity — a caller that does not set
   it still compiles and links, and now gets `CLAY_ERROR_INVALID_ARGUMENT`
   instead of a filled struct. Callers must add one line each:
   `cfg.struct_size = sizeof(cfg);`.

   The reason is that a descriptor a caller does not measure is one the library
   cannot bound: those three filled to `sizeof` as the LIBRARY defines it, and
   `clay_brick_config` had already grown a `colors` field, so every host built
   against the 24-byte layout had 8 bytes written past the end of its struct.
   Refusing the call is the only honest answer available — it does not rescue
   an already-compiled old host, which declares nothing and so cannot be
   served, but it turns silent corruption into a loud refusal. Rebuilding
   against 0.35.0's header is the fix for those hosts.
   **Neither is 0.24.1**: it changes no signatures at all. It corrects the
   swept guide's segment tie-break, so a scene containing a sweep can evaluate
   marginally differently at a guide corner — a behaviour change, not an ABI
   one. (0.24.0 was tagged and drafted but never published: it failed its own
   parity gate on a real OpenCL device, which is what 0.24.1 fixes. The tag
   remains for the record.)

   **0.24.0 is not such a release**: it is additive. Every signature that
   existed in 0.23.0 is unchanged, nothing was removed, and every field that
   existed keeps its offset, so code compiled against 0.23.0 keeps linking and
   behaving as it did. It adds 29 symbols, from three changes:

   - the `clay_brick_cache_*` surface, `clay_eval_grid`, and the node/layer
     influence bounds (`expose-the-brick-cache`);
   - the mask brush (`clay_mask_apply_stroke`), the bounded complement
     (`clay_mask_fill`, `clay_mask_invert_within`), the measured mask
     (`clay_mask_to_field`) and mask extrude (`clay_document_mask_extrude`,
     `clay_voxel_mask_extrude`) (`add-mask-stroke-brush`, `add-mask-extrude`);
   - tightened validation at the boundaries, which changes which inputs are
     REFUSED rather than which are accepted (`harden-core-boundaries`).

   Two descriptor structs GREW rather than staying byte-identical:
   `clay_relax_params` and `clay_flatten_params` each gained a trailing
   optional `mask`. That is the versioned-descriptor pattern doing its job —
   `struct_size` decides whether the field is read — so a caller compiled
   against 0.23.0 passes the shorter descriptor and gets exactly what it got
   before. It is called out here because "purely additive" is otherwise read as
   "no struct changed size", and a host that hard-codes a descriptor size
   rather than using `sizeof` would be surprised.
   `report-voxel-edit-effect` is the mildest kind of additive and does not make
   whichever release carries it a breaking one: it adds `clay_voxel_change_count`,
   changes no signature, removes nothing, and grows no struct. Every sculpt verb
   behaves exactly as it did, including the ones that can legally do nothing;
   what changed is that a host can now SEE that they did nothing, by reading the
   counter before and after rather than diffing the grid. No new `clay_result`
   value was added, and that was the point: an existing entry point returning a
   new non-zero code would turn a success into a failure for every caller
   already compiled.

   **0.24.2 carries two new symbols under a PATCH number**, which is worth
   stating plainly because nothing else in this list does it. It adds
   `clay_voxel_change_count` and `clay_layer_stroke_points`, and widens the
   placed-curve setter to accept a swept guide — an edit that previously
   returned an error and now succeeds. Every one of those is additive: no
   signature changed, nothing was removed, no struct grew, and code compiled
   against 0.24.1 keeps linking and behaving as it did. The direction that does
   NOT hold is downgrade — a host that builds against 0.24.2 and links 0.24.1
   gets undefined symbols, which a patch number would not normally warn anyone
   about. Read the symbol list rather than the number when pinning.

   **0.26.0 is such a release** — the third, after 0.2.0 and 0.22.0. Four
   `clay_brick_cache_*` entry points gained parameters rather than acquiring
   `_colored` / `_apron` / `_subset` siblings, following the precedent
   `clay_mesh_load` set in `close-c-abi-issue-gaps`: two entry points differing
   by one nullable argument would be two ways to say one thing.

   - `clay_brick_cache_read_bricks` gained `int32_t apron` after `count`, and
     `uint8_t* out_colors_rgba` + `size_t colors_capacity` at the end.
   - `clay_brick_cache_eval_requests` and `clay_brick_cache_submit` each gained
     a `float*` colour buffer and its capacity.
   - `clay_brick_cache_mesh` gained `keys_xyz`, `key_count` and `out_ranges`
     before its out-parameter.

   Every one is an ARITY change, so a caller compiled against 0.25.0 gets a
   **compile error rather than a misread** — there is no way for old code to
   link and behave differently, which is the property the never-silent rule is
   protecting. Passing `NULL`/`0` for each new argument reproduces the 0.25.0
   behaviour exactly, so the migration is mechanical.

   `clay_brick_config` GREW rather than changing: a trailing `int32_t colors`,
   under the `struct_size` rule, so a caller compiled against the older layout
   keeps the distance-only cache it had. `clay_brick_mesh_range` and
   `clay_vertex_layout` are new, and `clay_mesh_copy_vertices`,
   `clay_mesh_copy_indices` and `clay_brick_cache_raycast_many` are additive.

   It also carries one fix that changes RESULTS: `clay_brick_cache_mesh` with a
   NULL document produced no normals at all, though the header promised
   "positions and face normals" and `CLAY_NORMAL_FACE` says it needs no
   document. `mesh_bricks` applied attributes only through the tape. A host
   that relied on the documented behaviour was shading flat black; one that
   worked around it by computing its own normals now gets ours as well.

   **0.25.0 is additive.** It adds 23 symbols and four descriptor structs, and
   removes nothing. No existing signature changed, no existing struct grew or
   reordered a field, and no existing entry point returns a new `clay_result`
   value — so code compiled against 0.24.2 keeps linking and behaving as it did.
   Verified by comparing the header against the `v0.24.2` tag rather than by
   reading the changelog: 192 → 215 declared symbols, 21 → 25 structs, with the
   four new ones (`clay_field_report`, `clay_consolidation_params`,
   `clay_consolidation_cost`, `clay_mesh_layer_desc`) all new rather than grown.
   The five changes behind it:

   - **Mesh layers** (`add-mesh-layers`) — `clay_document_add_mesh_layer`,
     `clay_document_mesh_layer`, `clay_mesh_layer`, `clay_mesh_bounds`,
     `clay_mesh_uvs`. A document can now CARRY a mesh rather than only produce
     one.
   - **Host-buildable groups** (`expose-scene-groups`) — `clay_layer_add_group`,
     `clay_layer_add_item_in_group`, `clay_add_item_in_group`,
     `clay_item_add_child`, `clay_layer_children`. A sub-expression is sayable
     from C, with a new op value `CLAY_OP_INLINE = 255`.
   - **Consolidation** (`add-consolidation-policy`) — `clay_layer_consolidate`,
     `clay_layer_consolidation_cost`, `clay_layer_consolidation_state`,
     `clay_layer_field_report`. Collapses a layer to one redistanced volume, so
     a chain of bakes stops steepening.
   - **Multi-resolution voxels** (`add-multi-resolution-voxels`) — the seven
     `clay_voxel_*level*` calls, from `clay_voxel_add_level` to
     `clay_voxel_level_occupied_count`.
   - **Armatures** (`add-armature`) — `clay_layer_armature_edit`,
     `clay_item_set_armature_parents`, and a new primitive value
     `CLAY_PRIM_ARMATURE = 34`.

   Two new enum VALUES is the one thing worth reading twice. Neither changes an
   existing value, and no existing call can return them, so nothing compiled
   against 0.24.2 can meet one — but a host with an exhaustive `switch` over
   `clay_prim` or `clay_op` that it recompiles will get a new unhandled case,
   which is a compile-time nudge rather than a silent behaviour change.

   It also carries two bug fixes that change RESULTS rather than signatures, so
   a host may see different numbers from the same input:

   - A volume placed after any blob-carrying primitive (stroke, loft, swept,
     armature) in the same layer evaluated against the wrong payload —
     `ctape_volume` used offsets that are relative to the volume's own header as
     absolute indices into the tape blob, which coincide only at blob offset 0
     (#35). Every backend was affected; a document that hit this evaluates
     differently, and correctly, on 0.25.0.
   - FBX import welds rather than emitting a vertex per triangle corner (#38).
     An imported mesh had `triangle_count * 3` vertices whatever the file
     stored — six times the source on a typical model — and no two triangles
     shared a vertex. The surface is unchanged; vertex counts and anything
     derived from adjacency are not. `max_vertices` now bounds real vertices
     rather than corners, so a budget that previously refused a mesh may accept
     it.

   **`.clayspace` moves 1.4 → 1.7** across those changes: 5 adds the mesh
   chunk, 6 the voxel level stack, 7 an armature's parent indices. Minor 5 is a
   new chunk and so is skippable by a build that predates it; 6 and 7 are
   payload changes and are not — see the format notes at the top of
   `include/clay/io/clayspace.h`, which now spell out that "backward-open" means
   a current build opens older documents, not that an older build opens this
   one. Writing at an older minor is how a document is made readable by an
   older build.

   **Mesh layers (`add-mesh-layers`) are additive**, and landed in 0.25.0. They
   add five symbols —
   `clay_document_add_mesh_layer`, `clay_document_mesh_layer`,
   `clay_mesh_layer`, `clay_mesh_bounds`, `clay_mesh_uvs` — and one descriptor,
   `clay_mesh_layer_desc`. No existing signature changed, nothing was removed
   and no existing struct grew. `clay_document_mesh` still means "mesh the
   field" and returns exactly what it returned for the same document.

   The one behaviour change to an existing call is `clay_mesh_destroy` on a
   mesh obtained from a document layer, which is now a no-op rather than a
   free. That case could not previously arise — no entry point handed out a
   borrowed mesh — so no compiled caller can observe the difference. It is a
   silent no-op rather than the reported refusal `clay_voxel_grid_destroy`
   gives because the call returns no status, and changing its signature would
   break every consumer for a case nobody has.

   **`.clayspace` moves 1.4 → 1.5.** The major is unchanged, so nothing is
   refused on version grounds. The container gains a `MESH` chunk per mesh
   layer and the layer record's kind byte gains a third value. A reader written
   against 1.4 opens a 1.5 document, skips the unknown chunk, and ignores a
   layer whose kind it does not recognise exactly as it already ignores a voxel
   layer — and loses the mesh layers if it saves the document again, which is
   the same loss minors 1, 2 and 4 carry. The format notes at the top of
   `include/clay/io/clayspace.h` record it.

   **Unreleased is additive: two new symbols and one new enum.**
   `register-a-partial-backend` (#63, second half) adds
   `clay_backend_supports` and `clay_backend_diagnostic`, and the
   `clay_backend_op` enum they take. Nothing existing changes signature or
   behaviour.

   What DOES change is which backends register. A GPU backend now registers
   when its point and grid pipelines build, rather than requiring every
   pipeline it could provide; an operation whose pipeline is missing reports
   `false` from `caps()` and returns `Status::Unsupported`. On Apple
   Paravirtual GPUs — macOS VMs, which is what GitHub's macOS runners are —
   that is the difference between a Metal backend and no Metal backend at all.

   This is a behaviour change on exactly one axis and it is worth stating
   plainly: on such a machine `clay_list_backends` now answers `cpu,metal`
   where it answered `cpu`. A host that treated the presence of `metal` as
   "everything is available" and called `Backend::raycast` directly would now
   get `Unsupported` where it previously got a backend it could not find at
   all — a refusal it must already handle for `mesh()`, and one the parity
   suite has skipped on since before this change. No C ABI entry point is
   affected: `clay_raycast` and `clay_raycast_many` ask the registry for `cpu`
   by name, which is why the old rule was discarding a working backend over a
   kernel nothing could call.

   **Unreleased also adds QUAD MESHING, and the first thing to say about it is
   what it is not: a REGULAR QUAD GRID DERIVED FROM A SAMPLING LATTICE, NOT
   field-aligned retopology.** The quads follow the lattice and not the form —
   no edge loops around a limb or a mouth, no poles at features, density does
   not follow curvature, nothing animation-ready. It is the input a retopology
   pass replaces, not the output one produces. A host that ships it beside
   ZRemesher or QuadRemesher and calls it "remesh to quads" will have its users
   compare the two, so the header, the docstrings, the docs and
   `examples/44_quad_export.py` all say this and none of them says otherwise.

   **Additive, and nothing existing moved.** `mesh::Mesh` grew a `quads` array
   that is EMPTY on every mesh this library produced before — `indices` still
   holds exactly the triangulation of those quads, so decimation, the BVH,
   validation, the exporters, the C accessors and the mesh stream keep
   returning byte-identical results and were not modified. `clay_document_mesh`,
   `clay_voxel_mesh`, `clay_voxel_mesh_smooth` and `clay_voxel_mesh_chunks`
   return exactly what they returned before and carry no quads.

   Six new symbols, two new structs and one new enum:
   `clay_document_mesh_quads`, `clay_voxel_mesh_quads`, `clay_mesh_quad_count`,
   `clay_mesh_quads`, `clay_mesh_copy_quads`, `clay_mesh_quad_report`, with
   `clay_quad_params`, `clay_quad_report` and `clay_quad_mode`. No signature
   changed and no existing struct grew, so code compiled against 0.30.0 keeps
   linking; the minor bump this earns is SemVer for new surface, and it lands
   with the release commit as every other minor here does.

   **The count contract is the part to read before wiring a slider to it.** A
   target quad count is a HINT WITH A REPORTED ACTUAL — not a ceiling and not an
   exact count. The lattice cell size is the only lever, the count goes as
   `cell⁻²`, and it is not even monotonic in cell size (a finer lattice can
   resolve a thin feature the coarser one missed and ADD surface). So the
   mesher searches, lands inside about 5-10%, and reports what it produced
   through `clay_mesh_quad_report`; every iteration is a whole mesh, so
   `max_iterations` is a cost knob. Two nearby targets are two independent
   searches, so the count can move BACKWARDS as a slider moves forward — rare,
   documented, and not a bug. The voxel FACES mode is the exception to all of
   this: it has no cell size, so its search walks the grid's resolution levels
   from the coarsest and stops at the first that reaches the target. That one
   is monotonic, costs at most one mesh per level — and one for EVERY level up
   to the one that stops it, so a target met at level `k` reports `iterations`
   `k+1` and not the two of the bracket — ignores `max_iterations`, and reports
   `clamped` to mean the level STACK ran out: the target is below what the
   coarsest level that YIELDS ANYTHING gives, or above what the finest gives.
   The qualifier matters — a stack is not a strict mip, so a sculpt made only
   at a fine level leaves the coarse levels empty, and an empty level is not a
   level the caller can be handed.

   **`.clayspace` does NOT move for this.** The mesh stream carries quads as a
   tail APPENDED after the triangle indices, with no attribute-mask bit and no
   minor bump, precisely so an older build opens a document holding a quad mesh
   layer and reads it as the triangles it already is instead of refusing the
   whole document over a recoverable, redundant section. A tail that is present
   but malformed is refused, and the writer never emits one its own loader
   would reject.

   **GLB stays triangles.** OBJ, PLY and FBX write four-corner faces; glTF 2.0
   defines no quad primitive mode, so the GLB writer keeps writing the
   triangulation. "I exported GLB and got triangles" is the most likely bug
   report this feature can generate and it is not a bug, which is why it is
   stated at `clay_mesh_save`, in `mesh_io.h`, in the docs and in the example's
   printed output.

   **Unreleased carries a KERNEL DIALECT change and a format minor**, which
   makes it the first release since 0.24.x that asks anything of a host
   outside this repository. `volume-color-channel` gives a sampled volume an
   optional per-sample colour, because two shipped features were losing colour
   for the same structural reason: a `FieldVolume` had nowhere to put one, so
   consolidation baked a whole layer to a single node colour — a consolidated
   character came back without the distinction between skin and armour — and
   the voxel round trip converted once per palette entry to work around it.

   **What a host must do.** `docs/06-host-gpu-previews.md` ships the kernel
   headers so a host can sphere-trace our fields in its own shading language,
   with no second implementation to drift. The volume's blob grew a colour
   section and `ctape_prim_dist` gained a colour out-parameter, so **a host
   compiling those headers recompiles**. Nothing breaks silently: every
   section of a volume is addressed by offsets its header carries, so a host
   on the old headers reads the same distances it always did and simply never
   sees colour.

   **`.clayspace` moves 1.8 → 1.9.** The change is inside the volume's own
   blob rather than in the node record, so the older-reader story is the
   volume's: writing at minor 8 drops the colours and keeps the samples, the
   sparsity and the bounds, and an uncoloured volume — which is every volume
   any earlier build produced — loses nothing to it.

   **Consolidated output is no longer byte-identical to 0.30.0's**, and that
   is the point rather than a side effect. The bit-identity gate that guards
   the bake was not silently re-baselined: it compares the pooled grid bake
   against a serial full-tape bake, and the serial reference learned the same
   colour step, so the two still agree byte for byte.

   Additive at the C ABI: `clay_voxel_to_layer` keeps its signature but now
   produces ONE volume item carrying the palette where it produced one per
   entry, so a host counting nodes after converting counts differently. Said
   in the header rather than left to be discovered.

   **0.30.0 is additive: nine new symbols and one new struct, no signature
   changed, nothing removed, and no existing struct grown.** Code compiled
   against 0.29.1 keeps linking and behaving as it did; the minor bump is
   SemVer for new surface, not a warning.

   - **A layer's nodes enumerate** (#91): `clay_layer_node_count` /
     `clay_layer_node_at`, the node-level sibling of the layer pair 0.29.0
     added. `clay_layer_children` answers for a *group*, and a layer's root is
     not a group, so before this the only way to find a placed armature was to
     probe node ids upward and tolerate a run of misses — ids are not dense and
     nothing bounds a gap. Top level only, deliberately: the whole tree is
     walked by pairing these with `clay_layer_node_prim` and
     `clay_layer_children`, which keeps the nesting a host needs to draw an
     outliner.
   - **A layer can be renamed** (#92): `clay_document_set_layer_name`, the
     setter for the getter 0.29.0 added. It is a command, so it is one undo
     step. Duplicate names are permitted, because all three create calls
     already permit them and refusing on rename alone would buy a uniqueness
     the document never had; `clay_document_voxel_layer` answers with the first
     layer in stack order, and the header now says so. An empty name is
     refused — a cleared text field should not destroy the name it replaces.
   - **Per-node armature signs** (#99): `clay_item_set_armature_signs` and
     `clay_layer_armature_signs`, so a ZSphere rig can carry negative nodes.
   - **The voxel display path is incremental** (#86 part 2):
     `clay_voxel_take_dirty_chunks`, `clay_voxel_mesh_chunks` and the
     `clay_voxel_chunk_mesh_range` array element — the voxel side of the
     `mark_dirty → take_dirty → mesh` shape the brick cache already had.
     `clay_voxel_mesh` keeps meaning "mesh the whole grid", keeps its
     signature, and keeps its output byte-identical, so it remains the export
     path with the tighter merge.

   **`.clayspace` moves 1.7 → 1.8**, carrying the per-node armature signs. The
   major is unchanged, so nothing is refused on version grounds: a reader
   written against 1.7 opens a 1.8 document and loses the signs if it saves it
   again, the same loss the earlier minors carry.

   **0.30.0 also gets much faster at showing a voxel sculpt, without changing
   what it shows.** Two changes, both gated on byte-identity rather than on
   inspection, figures from one Linux desktop measured back to back and
   therefore ratios rather than device numbers (`docs/RELEASE.md` forbids
   comparing them against the device baseline):

   - **The mesh sweep stops probing the chunk map per cell** (#86 part 1):
     4.12 → 0.157 ms per occupied chunk, ~26×, and a realistic sculpt at the
     device fixture's cell size 396 → 17.6 ms. Pure refactoring behind an
     unchanged result: an independent check of 157 fixtures — negative
     coordinates, every chunk seam, non-active levels, chunks erased to zero
     occupancy, a saturated palette, random soaks — moved not one byte.
   - **A dab re-meshes only what it dirtied** (#86 part 2): 0.65 ms against
     23.3 ms whole, ~36×, which puts the voxel path in the same order as the
     brick cache's 0.64 ms against 22.6 ms. It costs +3.7% triangles at chunk
     seams, and dirty tracking costs the write path ~16% on a fill-heavy run
     (+0.0012 ms on a size-8 stamp). Per-chunk meshing cannot crack the
     surface — greedy quads are axis-aligned and exact, so clamping the merge
     to a chunk boundary splits a quad — and a test proves it by decomposing
     both meshes into unit faces rather than comparing triangle counts.

   **The device gate now measures the display path and the level stack** (#86
   part 3, #89): five cases — whole-grid meshing, incremental meshing, a
   subdivide, a verb with a level under it, and a verb at radius 32 where every
   other voxel case is radius 8. Their budgets are hand-set ceilings carrying
   the PROVISIONAL note and **must be re-seeded with `--update` on the
   reference device**; until that run they are ceilings, not results. This
   closes the gap that let a ~130× display cost sit beside edit verbs
   reporting two orders of magnitude of headroom.

   The `clay_brick_cache_mesh_lod` note below is part of this release too.

   **`mesh-brick-cache-lod` (#93) adds `clay_brick_cache_mesh_lod`**, which is
   `clay_brick_cache_mesh` plus an `int32_t lod` before the key list — the
   position `lod` holds in `clay_brick_cache_read_bricks`. The existing
   entry point keeps its signature and forwards at lod 0 through the SAME
   body, so code compiled against 0.29.1 keeps linking and producing the same
   bytes. Verified rather than asserted: a fingerprint program linked against
   0.29.1's `libclay_shared.so` and against this tree's produces identical
   surface-brick counts, key order and buffer hashes for the whole-cache and
   key-subset lod-0 meshes, with and without a document.

   This is deliberately NOT the arity change `close-webgpu-host-abi-gaps` made
   to the same call. That precedent rests on "two entry points differing by one
   nullable argument would be two ways to say one thing", and a level is not a
   nullable argument: it changes what `keys_xyz` MEANS (fine keys at lod 0,
   coarse block keys at lod 1) and brings refusals that apply to one of its
   values only — an unbuilt level is `CLAY_ERROR_NOT_FOUND`, and colours and
   gradient normals are refused. `clay_brick_cache_mesh_lod` also returns
   `CLAY_ERROR_NOT_FOUND`, which `clay_brick_cache_mesh` never did and still
   does not: the new code is on the new symbol, so no already-compiled caller
   can see a success turn into a failure.

   **0.29.1 changes no signature, adds no symbol, and changes no result — it
   is speed only**, which is why it is a patch under the same rule that made
   0.27.3 one. Seven changes, each merged with a measured A/B and a
   bit-identity gate (same inputs, byte-identical outputs against 0.29.0),
   figures from an M2 Max, medians, driven through the C ABI:

   - **A per-revision cull index + per-batch coarse cull** (#82): per-brick
     culled tape compiles stop walking the whole document — 59.6 → 6.0
     ns/item, and a dab's refill slope on Metal drops 2.22 → 0.21 µs/item.
     Emitted tapes are byte-identical by the cull contract's superset rule.
   - **Batched brick-mesh attribute taps** (#83): gradient normals + colors
     evaluate through the CPU pool in one batch — the mesh share of a dab
     goes from 6.7× the refill slope to 1.14×; a dense re-mesh at 10 000
     stamps drops 98.8 → 15.0 ms.
   - **The zero-copy device refill is batched** (#84): it had missed #64's
     treatment and ran 28–166× SLOWER than the host-memory route; it now
     runs the same chunked pipeline into the caller's buffers at 0.7–1.0×
     the host route, values identical to the bit.
   - **Consolidation bakes on the thread pool** (#85): 351/958/3094 ms at
     100/300/1000 items became 87/160/426 ms (4–7.3×), volume byte-identical;
     the estimate stays the bake itself. A per-brick-culled variant was
     tried, measured, and rejected for a real reason — band-clamped identity
     is not raw-sample identity — recorded in consolidate.cpp.
   - **The Metal backend keeps scratch buffers and uploaded tapes resident**
     (#88): steady-state eval of a document carrying a consolidated volume
     stops re-paying the blob upload (flat in blob size, ~5× at 22 MB), a
     1-brick call costs ~0.17 ms against the 0.52 ms it did, and the
     CPU/Metal crossover moves to ~8 bricks/call. Its third piece — a 2 ms
     status-poll before parking on small dispatches — shipped macOS-only
     after the DEVICE GATE caught it regressing the iPad 3.1×
     (`sdf_stamp_metal` 1.77 → 5.54 ms p95): an iPad's CPU and GPU share one
     power budget, so the spinning core starves the kernel it waits on. The
     Mac keeps the measured win; iOS parks immediately, as it always did.
     That failure is the device gate doing precisely what it was built for —
     a desktop-verified micro-optimisation that inverts on the hardware that
     ships.
   - **Batched brick raycasts fan out across the worker pool** (#94):
     ~40 → ~320 rays/ms on twelve cores, results byte-identical in order.
   - **Undo stops scaling with the document** (#95): node removal indexed —
     undoing a 100-stamp stroke at 10 000 stamps drops 0.62 → 0.008 ms, flat
     in document size, serialized state byte-identical through undo/redo.

   The engine's C++ evaluation interface grew two internal batch entry
   points (`eval_points_batch`, `eval_grid_batch_device`) with
   identical-results loop defaults — internal headers, free to evolve within
   the major, invisible at the C ABI.

   **0.29.0 adds six symbols, one enum and one descriptor, and removes
   nothing.** No existing signature changed, no existing struct grew or
   reordered a field, and no existing entry point returns a new `clay_result`
   value, so code compiled against 0.28.0 keeps linking and behaving as it
   did. Two changes:

   - **Layer enumeration** (`expose-layer-enumeration`, #69) —
     `clay_document_layer_count`, `clay_document_layer_at` (index is STACK
     position, which is evaluation order), `clay_document_layer_info` (a
     `struct_size`-leading output descriptor, `clay_layer_info`: id,
     representation, stack index, visible, ghost, locked) and
     `clay_layer_name` (the ABI's size-query string pattern), plus the
     `clay_layer_representation` enum. Pure exposure: the document model and
     `.clayspace` already round-tripped every field, so the format is
     untouched. This closes the reload gap where a host probed ids against
     `clay_layer_bounds`, regenerated names, mistook voxel layers for SDF and
     — the correctness half — lost stack order, so a reopened document could
     evaluate differently from the one saved.
   - **Armature readback** (`read-armature-tree`, #77) —
     `clay_layer_armature_parents` (the `clay_layer_children` size-query
     shape, counted in nodes; a tree authored with fewer parents than points
     reads back padded with roots, exactly as evaluation reads it) and
     `clay_layer_node_prim` (which primitive a placed node carries; refuses
     groups, the dual of `clay_layer_children`). One existing call accepts an
     input it refused: `clay_layer_stroke_points` now serves the xyzr half of
     `CLAY_PRIM_ARMATURE` instead of returning `CLAY_ERROR_INVALID_ARGUMENT`
     — the 0.24.2 kind of additive, an error turning into a success, with no
     signature change. The placed-node point SETTER still refuses armatures:
     points replaced alone would desync from parents.

   It also carries a speed fix that changes no results: brick-mesh gradient
   normals and colours are evaluated against a tape culled per brick (#73),
   the same culling refill uses, inside which band-clamped evaluation is
   bit-identical — so a fixed 80-brick re-mesh stops scaling with the
   document (130 ms → 7.7 ms at 193 nodes, ~flat in document size, gated in
   CI by a bench ratio). And a build fix (#71): all 40 parenthesized
   aggregate initialisations of `math::Aabb` — a C++20 P0960 form AppleClang
   15 rejects — became braces, and a pinned Xcode 15.4 CI row now gates the
   AppleClang 15 toolchain floor recorded in `docs/05-claycore-library.md`.

   **0.28.0 is additive: one symbol and one grown descriptor**
   (`add-feathered-volume-replace`, closing #67). `clay_item_volume_relax_from`
   mirrors `clay_item_volume_flatten_from` — a document-sourced relax, so the
   last SDF verb stops requiring a bake-first round trip. `clay_volume_params`
   gains a trailing `feather`: a volume placed with `CLAY_OP_REPLACE`
   crossfades into the field beneath it over that margin instead of meeting it
   at a hard box edge, which is what removes the cell-wavelength normal
   corrugation of the bake round trip. The versioned-descriptor pattern
   applies: a zeroed feather, and any pre-0.28 struct_size, is the hard
   replace byte for byte. Internally the tape gains one combine mode
   (`ccombine_replace_feather`) and the volume blob header grows to 13 floats,
   both by their existing self-describing rules; a `.clayspace` written by
   0.27 loads with feather 0 and one written by 0.28 loads in 0.27 with the
   feather ignored.

   **0.28.0 also changes RESULTS in three places, none of them silently.**
   No signature changed for any of these; what changed is what the same input
   produces, which is exactly what the never-silent rule exists to record:

   - **`CLAY_OP_ADD` honours `clay_stroke_preset.strength`** (#61). It never
     had: a stroke at strength 0 deposited the same full stamp as one at 1,
     because strength was consumed only where `blend.k` is an amplitude
     (Relief, Incise). An add stamp's whole deposit now scales with strength —
     1.0 is bit-identical to before, 0 authors no node, monotonic between, and
     a clamped add stroke still equals a buildup one. A host that mapped an
     intensity slider onto strength and shipped around the flat response will
     see the slider start working.
   - **The layer mirror mirrors the layer by default** (#60).
     `clay_set_layer_mirror` stored the plane, but evaluation mirrored only
     items whose per-item participation flag opted in — and that flag
     defaulted to excluded, so the sequence every host writes (set the mirror,
     add items) mirrored nothing. Participation now defaults to mirrored:
     `clay_item_desc.mirror` and `clay_item_set_mirror` read 0 and 1 as
     "follow the layer" and a negative as "excluded". The one input that is
     re-read: 0, which previously meant excluded — and was also the zeroed
     descriptor, which is precisely how the feature came to be dead. Saved
     documents carry the flag per node and load unchanged; a layer with no
     mirror set compiles a byte-identical tape, pinned by test, so stroke
     latency is untouched.
   - **A subset brick mesh emits the straddlers it owes** (#66).
     `clay_brick_cache_mesh` with a key list omitted triangles reaching a
     corner across the request boundary, so no sequence of subset calls could
     reconstruct a complete surface — dilating the request only moved the
     boundary. A subset now returns every triangle with at least one corner in
     a requested brick, each attributed to the lexicographically lowest
     requested key owning a corner so a per-brick host can dedupe. The
     whole-mesh (NULL keys) path is byte-identical, and a subset naming every
     surface brick equals the whole mesh.

   **And one speed change**: `clay_brick_cache_eval_requests` reaches the
   backend as one batch instead of one call per brick (#64). The Metal path
   paid a full GPU round trip per brick — its own command buffer, five
   allocations, a tape upload and a `waitUntilCompleted` for 512 samples —
   which is why it sat 7–10× BEHIND the CPU at every batch size. Batched into
   one dispatch, a 17576-brick whole-model refill on an M-series Mac goes from
   3.5 s to ~10 ms (from 10× slower than the CPU to ~30× faster), a 27-brick
   dab from 6.5 ms to 0.35 ms, and the CPU/Metal crossover sits near 16 bricks
   per call — measured guidance now in `clay.h`, replacing the old "pass cpu
   here" advice. Other backends inherit a loop-based default and are
   byte-for-byte unchanged. The relief amplitude contract is also now
   documented and pinned by test (#62): displacement tracks `blend_k` up to
   the region's extent (radius + rounding) and saturates below
   radius + 2·rounding, so rounding buys amplitude as well as softness.

   **0.27.3 fixes the Metal backend on paravirtualised GPUs, and makes it
   faster on real ones.** No signature changed and nothing was added; this is a
   kernel change and a diagnostic.

   `clay_raycast` would not build a compute pipeline on an Apple Paravirtual
   device — a macOS VM, which is what GitHub's runners are — so the whole Metal
   backend was discarded and the library fell back to the CPU without a word.
   It is the only one of the three kernels that evaluates the tape more than
   once (per march step, plus four times for the normal), so inlining
   `ctape_eval` at each site made it large enough for the pipeline compiler to
   give up, with an error whose entire content was "Compilation failed".
   `TapeField::operator()` is `noinline` now, in the Metal backend's own file;
   the shared kernel headers are untouched.

   Measured on an M2 Max through `Backend::raycast()`, 200k rays against 40
   blended spheres: **45.16-47.00 ms inlined, 37.56-37.80 ms after** — about
   20% quicker with an identical hit count, because the inlined version cost
   more in register pressure than the call costs.

   **Metal parity now runs in CI for the first time.** It had been skipped on
   every previous run, which is how the backend came to be shipped unverified
   there. And when Metal init fails it now says why, on stderr, naming the
   stage and the function — the ignored `NS::Error**` behind three failed
   release attempts. Issue #63 stays open for the half that is not fixed: one
   failing pipeline still disables the entire backend, and `clay_list_backends`
   returning `cpu` is still indistinguishable from a build without Metal.

   v0.27.2 was tagged and its release failed on this; the tag remains for the
   record, as v0.27.0's and v0.27.1's do.

   **0.27.2 is 0.27.1 plus one test fix, and changes no library code either.**
   v0.27.1's release workflow got past the checklist and failed building the
   xcframework: the Swift smoke asserts that Metal REGISTERS, and a GitHub
   macOS runner has no Metal device, so `MetalBackend::create()` returns null
   and the backend legitimately does not register. The assertion was true of a
   developer's Mac and false of the runner the release runs on, which made the
   release unshippable.

   What the ARTIFACT ships is a different question from what a MACHINE gets,
   and only the first is decidable without a GPU. The first is already gated,
   harder, in `tools/build_xcframework.sh`: the build fails outright if a
   slice's merged archive carries no `clay_metallib` symbol. So the smoke now
   asserts registration where a Metal device exists and prints a SKIP naming
   the reason where one does not. The v0.27.1 tag remains for the record.

   **0.27.1 is 0.27.0 plus one CI fix, and changes no code at all.** v0.27.0 was
   tagged and its release workflow failed its own device gate — not on the
   library, on the clone: `actions/checkout` defaults to depth 1, the gate
   compares HEAD against the commit its recorded run names, and a shallow clone
   cannot resolve that commit. The gate said so ("cannot diff against the gated
   commit… shallow clone?") and failed rather than passing on a claim it could
   not check, which is the behaviour that change asked for. The checklist job
   fetches full history now. **The v0.27.0 tag remains for the record**, as
   v0.24.0's does, and nothing was published from it.

   **0.27.0 adds four symbols**, from two changes, and is additive throughout:
   no signature changed, nothing was removed, no struct grew, and no existing
   entry point returns a new `clay_result` value, so code compiled against
   0.26.0 keeps linking and behaving as it did.

   **A merged export** (`add-mesh-layers` 4.6/4.7, issue #54) —
   `clay_mesh_transform`, `clay_mesh_concat` and `clay_document_mesh_combined`.
   `clay_document_mesh` still means MESHING THE FIELD and is bit-identical on a
   document that has mesh layers, so combining is a separate call rather than a
   change of behaviour in that one. A hidden mesh layer is excluded from the
   combined export; ghost and lock are not, consistent with neither flag
   changing what a document evaluates to. Concatenation rebases indices and
   DROPS an attribute present on some inputs and absent on others, because the
   alternative is a mesh whose uvs are non-empty and a different length than its
   positions — malformed, and discovered in an exported file rather than at the
   call.

   **A document-sourced flatten** (issue #55) — `clay_item_volume_flatten_from`,
   a flatten sampled from a document rather than from an existing volume. It
   exists because the sound path was Python-only: `pyclay` has both
   `Volume.flattened` (a volume) and `Volume.flattened_from` (a source plus its
   own sampling parameters), and the C ABI had only the first. Measured, the two
   produce the SAME surface — same facet position, same enclosed volume, at
   every band tried — and differ by about 8x in `safe_step_scale`, so the
   document-sourced field costs a fraction of the marching for the same shape.
   `tools/check_binding_parity.py` used to map both Python names onto the one C
   symbol, which is how the gap passed the gate; it maps one symbol per
   operation now.

## The device gate

Metal is the iPad app's production path, and no CI runner has an attached
iPad. So the one check that covers the path the app ships on cannot run in
CI — it runs here, on hardware, before the tag.

**It is not optional and it does not skip.** A skipped hardware gate and a
passing one are indistinguishable in a log, which is exactly how "Metal is the
iPad app's production path" reached v0.25.0 without a single iPad ever having
run it.

### Prerequisites

- **An attached iPad with Developer Mode enabled.** `xcrun xctrace list
  devices` must list it above the `== Devices Offline ==` heading; a
  paired-but-absent device is listed by name and cannot be run on.
- **A signing identity and a provisioning profile that covers the device.**
  Today that is the Cyberdyne team, `2C69VJZSNR`, whose wildcard profile
  (`iOS Team Provisioning Profile: *`) covers the lab devices. **The signing
  certificate in use expires 2026-09-02.** An expired certificate blocks the
  gate and therefore blocks the release, so renewal is on the release critical
  path rather than being somebody's background chore.

  Check the certificate **inside the profile**, not the first one in the
  keychain — this machine also holds an unrelated, already-expired
  `Apple Development` identity on a different team, and
  `security find-certificate` returns that one first:

  ```sh
  for p in ~/Library/Developer/Xcode/UserData/Provisioning\ Profiles/*.mobileprovision; do
    security cms -D -i "$p" 2>/dev/null | python3 -c '
import sys, plistlib, subprocess
d = plistlib.loads(sys.stdin.buffer.read())
print(d["Name"], "| team", d["TeamIdentifier"][0], "| profile to", d["ExpirationDate"])
for c in d.get("DeveloperCertificates", []):
    r = subprocess.run(["openssl", "x509", "-inform", "DER", "-noout", "-enddate"],
                       input=c, capture_output=True)
    print("   cert", r.stdout.decode().strip())'
  done
  ```

  The wildcard profile itself is valid until 2027-06-17; the certificate
  inside it is the earlier deadline, and the one that matters.
- **`xcodegen`** (`brew install xcodegen`). The Xcode project under
  `tests/device/` is generated from `project.yml` and is not committed: a
  pbxproj is not reviewable, and generating it on every run keeps the spec and
  the project from drifting.

An Xcode project exists at all only because XCTest has no hostless mode on a
device destination — `xcodebuild` refuses with "Select a host application for
the test target" — and SwiftPM cannot declare a test host. The host app under
`tests/device/Host/` is empty and exists solely to satisfy that.

### The reference device

**`iPad15,5` (iPad Air 13-inch, M3) on iOS 26.5.2** produced the committed
baseline. A run from any other model or OS is **refused rather than compared**:
the numbers are not commensurable, and scoring them against this baseline
would produce a figure that means nothing. Moving to a different reference
device means re-taking the baseline on it, deliberately, as its own commit.

### Running it

```sh
tools/run_device_bench.sh                        # first attached iPad
tools/run_device_bench.sh <udid>                 # a specific one
python3 tools/check_device_bench.py build/device/device-bench.json
python3 tools/check_device_coverage.py build/device/device-bench.json
```

`run_device_bench.sh` rebuilds the xcframework first rather than trusting what
is on disk. This repo has been bitten by that exact staleness before: the
Swift smoke consumes the prebuilt xcframework rather than the working tree, so
it had been passing against an old one while the tree moved underneath it
(found by `add-mesh-to-field-import`).

`check_device_bench.py` writes `tests/device/last-gate.json` on success,
recording the commit it passed against. `tools/release_check.py` reads that
file and **fails the release when anything under `src/`, `include/`,
`backends/`, `bindings/` or `CMakeLists.txt` has changed since** — so the
release can require the gate without an iPad being attached to CI. A docs or
spec commit does not invalidate it.

### Reading a result

Each case reports **p50 and p95 in milliseconds** at three document sizes
(10 / 100 / 1000 accumulated stamps), plus a `growthExponent` — the log-log
slope of cost against document size. `0` is flat, `1` is linear, `2` is
quadratic.

Three failures mean three different things:

| Failure | Means |
|---|---|
| `REGRESSION` | slower than the committed baseline by more than tolerance |
| `BUDGET` | slower than the interaction class allows, regressed or not |
| `GROWTH` | cost is scaling faster than the document (over `N^1.25`) |

And two refusals, which are not scores at all: a run from **different
hardware**, and a run that was **thermally throttled**. `ProcessInfo`'s
thermal state is sampled at both ends and anything but `nominal` invalidates
the run. This fires in practice — several harness runs back to back will take
an iPad to `serious`. Let it cool and run again rather than reaching for the
tolerance.

**Simulator and Mac numbers are not device numbers and must never be compared
to this baseline.** A Mac has more cores, active cooling and no
memory-pressure kills. The `metal` CMake preset on a Mac is the right tool for
"does the Metal backend agree"; it is the wrong tool for any question about
latency.

Budgets live in `tests/device/baseline.json`. `budgetMs` is a ceiling on p95
at the worst point of the axis — what the engine must not exceed, which is
**not** the same as what it should cost. Where those differ the entry carries
a `note` saying so. `sdf_stamp_cpu` is the live example: it is already outside
the engine's half of a 120 Hz frame, its budget is a regression ceiling rather
than an endorsement, and the checker reprints the breach on every run so
writing it down does not retire it.

## Tagging

```sh
git tag -a v1.0.0 -m "claycore v1.0.0"
git push origin v1.0.0
```

`.github/workflows/release.yml` then re-runs the checklist on Linux, builds
wheels for macOS/Linux/Windows via cibuildwheel, builds the
`claycore.xcframework` with its SwiftPM checksum, packages
`claycore-kernels.zip` (the kernel headers plus the host parity fixture — see
`docs/06-host-gpu-previews.md`), and opens a **draft** GitHub release with
everything attached. Drafts are deliberate: review the artifacts before
publishing.

A host consuming the kernels artifact pins it by release tag, so a release
that changes kernel math changes the fixture too: regenerate and re-run it on
the host side rather than assuming the previous one still passes.

## Open items before v1.0

Tracked honestly rather than assumed done:

- **The xcframework shipped CPU-only until this was fixed** (issue #45).
  `CLAY_BACKEND_METAL` defaults off and `build_xcframework.sh` never passed it,
  so every Apple host linking the shipped artifact got `clay_list_backends ->
  "cpu"` and no way to opt in: the option decides what is COMPILED INTO the
  archive, and a consumer of a prebuilt static library cannot add a backend
  afterwards. The consuming app measured 2.6x at 25 items on an iPad Air M3,
  with CPU degrading 2.7x over eight strokes while Metal stayed flat.

  Two things are worth carrying forward. The first is that a flag flip alone
  would NOT have fixed it: the metallib was compiled `-sdk macosx` regardless
  of the slice, so the iOS slices would have carried a macOS metallib, which
  links cleanly and fails to register at runtime — the same CPU-only outcome,
  one level harder to see. The second is that nothing asserted any of this, so
  it survived several releases; there are now two gates, the build failing on a
  slice with no embedded metallib and the Swift smoke test asserting the
  backend registers.

  **Still unverified on hardware.** The fix was written on a machine with no
  Apple toolchain. The release workflow builds the xcframework on macOS and now
  fails rather than shipping a CPU-only slice, but "`clay_list_backends`
  reports metal on a real iPad" has not been observed by this repository.

- **A Metal parity deviation was reported on the iOS Simulator, not on device**
  (issue #45, recorded by the consumer as a note rather than a bug report). A
  Move-Topological fixture measured the baked field departing 0.166 from the
  document where CPU gives 0.033; it did not reproduce on device and the
  rendered goldens were unchanged. Not dismissed: "the simulator emulates
  Metal" is an explanation, not evidence. The thing to do is reproduce it under
  the parity suite on the simulator rather than through an app fixture, which
  only became possible now the framework carries Metal at all.

- **Vulkan device parity has executed** (added with the backend). Measured with
  the undilutable metric rather than the aggregate: `parity: every registered
  backend matches the scalar reference` reports **204** assertions with the
  Vulkan runtime hidden (`VK_DRIVER_FILES=/nonexistent`) and **408** with it
  registered — exactly 2x, one pass each for cpu and vulkan, both matching the
  scalar reference. Validated on an RTX 5060 (driver 580, Vulkan 1.3.275) and
  again on **lavapipe**, the software runtime, which gives the identical count.

  Those two runs do not prove the same thing and must not be quoted as if they
  did. lavapipe executes on the CPU, so its agreement with the CPU reference is
  close to guaranteed by construction: it gates the *plumbing* — SPIR-V
  validity, descriptor and buffer layout, dispatch, readback — and says nothing
  about arithmetic. Only the RTX 5060 run is evidence about arithmetic, and a
  release that touches kernels needs a real device for it, exactly as CUDA and
  OpenCL do.

  What is NOT yet measured is where this backend's CPU/GPU crossover sits. The
  16³ figure in the brick-cache note below is a Metal-on-M2-Max number; Vulkan's
  dispatch cost is its own and has to be found rather than inherited.

- **The Vulkan backend does gradients on the host.** `eval_points` runs on the
  device; a request for gradients falls back to the scalar reference for the
  whole batch, because the tetrahedron tap lives in `field.h`, which is
  templated C++ that no compute dialect in this tree compiles. Same choice the
  OpenCL backend makes, recorded here rather than left to be discovered from a
  profile. A caller asking a tier-3 backend for gradients is paying CPU for them.

- **CUDA and OpenCL device parity have both executed** (task 12.2, closed by
  `fix-cuda-arch-selection`; re-run for v0.24.0 on 2026-08-09, and again for
  v0.25.0 on 2026-08-10 because that release adds a tape opcode — an armature —
  and a new opcode is exactly what a CPU-only parity run cannot vouch for).
  The v0.25.0 re-run isolates the case that actually loops the registry rather
  than reading the whole gate's total, which is dominated by tests that do not:
  `parity: every registered backend matches the scalar reference` reports **204**
  assertions CPU-only and **612** with both devices registered — exactly 3x, one
  pass each for cpu, cuda and opencl, all matching the scalar reference. Prefer
  that measurement to the aggregate below; it cannot be diluted. Validated on an
  RTX 5060 / driver 580 / nvcc 12.0, configuring one build directory with
  `-DCLAY_BACKEND_CUDA=ON -DCLAY_BACKEND_OPENCL=ON` and pointing
  `tools/release_check.py --build-dir` at it. Both backends register and match
  the scalar reference. pyclay at 1M points matches the CPU bit-for-bit, which
  is expected from the same single-source headers compiled `-fmad=false`.

  Read the differential in **assertions, never test cases**. The parity loops
  run *inside* the doctest cases, so `-tc=*parity*,*registry*` reports 11 cases
  whether or not a GPU backend registered — a CPU-only run reports 11 too, and
  a backend that failed to register passes vacuously. Only the assertion count
  moves:

  | Registered backends | Assertions | Added by the backend |
  |---|---|---|
  | CPU only | 836,831 | — |
  | CPU + CUDA | 838,909 | +2,078 |
  | CPU + CUDA + OpenCL | 840,796 | +1,887 |

  Hide a device to take the control run: `OCL_ICD_VENDORS=/nonexistent` for
  OpenCL. Note that `CUDA_VISIBLE_DEVICES=""` hides **both** where the NVIDIA
  ICD is the only one installed, so it is not a CUDA-only control.

  (Earlier revisions of this file recorded 1115 against 479. Those numbers
  predate `fadb595`, which extended the host parity fixture to the current
  kernel set and grew the counts by roughly 750x. The differential logic was
  unchanged; only the absolute figures were stale.)

  *Caveat*: validated through PTX JIT only. A cubin build for sm_120 needs
  CUDA >= 12.8, so that path is still unexercised.
- **CI no longer builds CUDA or OpenCL at all** (changed 2026-08-07). Neither
  runner has the hardware that would make those jobs mean what their names
  said: the CUDA job compiled against no device, and the OpenCL job ran parity
  against pocl, whose arithmetic *is* the CPU's — so it agreed with the CPU
  backend almost by construction. What still gates every push is
  `check_kernel_dialect.py`, which compiles every kernel header under the CPU,
  CUDA and Metal profiles plus the OpenCL amalgamation, so a dialect break
  fails in seconds on any runner.

  The consequence is that **four things are now manual and hardware-dependent**
  rather than gated, and all four must be run before a release that touches
  kernels:
  1. CUDA device parity (as below).
  2. The nvcc build of the backend, including its architecture auto-detection.
  3. That the OpenCL backend registers and passes parity on a real device.
  4. That the Vulkan backend registers and passes parity on a real device — a
     lavapipe run is not a substitute, for the reason given in the Vulkan entry
     under "Open items": it executes on the CPU, so it gates plumbing rather
     than arithmetic. The `vulkan-plumbing` CI job runs lavapipe on every push
     and covers the other half — that the shaders compile, that the backend
     registers, that dispatch and readback work — so what stays manual here is
     specifically the arithmetic on real silicon. That job asserts registration
     explicitly, because the loader left to itself picks a device the backend
     cannot use and the suite then passes CPU-only at 204 assertions instead of
     failing.

  5. That **Metal device adoption** works — `clay_device_adopt` with a
     `MTLDevice` and `MTLCommandQueue` the caller made, then
     `clay_eval_grid_device` into a caller-owned `MTLBuffer`, compared against
     the host-memory path. Added with `add-device-interop` and written on a
     Linux machine with no metal-cpp toolchain: CI compiles it on every push
     and the Metal parity job exercises the ordinary path, but the ADOPTION
     path has never run on Apple hardware. The Vulkan equivalent is covered by
     the `vulkan-plumbing` job and by the unit suite; this one is not.

  `python3 tools/release_check.py` run on a machine with those devices present
  covers the first four, because it runs parity against every backend registered in
  that build. What per-push CI still gates for Vulkan is
  `check_kernel_dialect.py`, which compiles the generated GLSL with glslang and
  needs no device — the strictest of the five profiles, so it usually fails
  first when a kernel gains something new.
- **CUDA-enabled wheels are not shipped** (task 12.3). Wheels currently carry
  the CPU backend, which per the parity contract changes speed, not results.
  Shipping CUDA wheels needs a CUDA build host in the wheel matrix.
- **Blender-headless FBX validation** runs only as a release-time manual
  check; per-push CI validates exports with assimp instead (task 9.3).
- **The brick cache is exposed and now timed on Apple silicon, but not on a
  tablet** (added 0.24.0, measured 2026-08-08). The design's premise was that
  `eval_bricks` goes to Metal while the per-brick tape compile stays on the CPU.
  Measured on an M2 Max (8P + 4E, macOS 26, `--preset metal`), **that premise is
  wrong for bricks**: a brick is 8³ = 512 samples, too little work to cover a
  dispatch and a per-call allocation, so Metal costs 288 µs per brick against
  the CPU's 114 µs and never wins at any thread count. Sweeping the grid size
  puts the crossover at 16³ — from there Metal wins, reaching 10× at 32³ and
  20× at 128³. The header's advice to fan out over requests one brick per
  worker *is* now measurement: it takes a 216-brick fill from 24.7 ms to 8.2 ms
  on twelve workers, a 3.0x speedup. Metal and the CPU agree exactly over a full
  fill (max abs difference 0.0), so this is a speed result, not a parity one.

  **0.28.0 retires the "keep refill on cpu" half of that advice** (#64): the
  per-brick round trip was the cost, not the brick. `eval_requests` now
  reaches the backend as one batch, and the crossover for refill sits near 16
  *bricks per call* on the same class of machine — route by batch size, per
  the guidance in `clay.h`. The per-brick culled-tape compile still happens on
  the CPU either way.

  What is still untested is a *tablet*. An M2 Max has twelve cores, active
  cooling and a 34 GB unified pool; an iPad has fewer cores, a hard thermal
  ceiling and a memory budget that kills apps rather than swapping. The fan-out
  gain assumes cores that stay at clock, and both crossovers — 16³ for grids,
  ~16 bricks for a batched refill — are the numbers most likely to move on a
  GPU with a different dispatch cost. Re-measure on the target iPad before
  wiring up the split.
- **A brush dab's brick count is flat, and its cost is now nearly so**
  (added 0.24.0, **corrected by measurement**). The count claim holds as
  designed and as tested: holding density constant while the document grows from
  100 to 2400 items, a dab keeps dirtying 22–24 bricks.

  Its *time* used to grow with it — 2.6 ms to 8.8 ms over that range — because
  `clay_brick_cache_eval_requests` compiled a culled tape per brick and each
  compile walked every node in the document. `CullIndex` (bounds computed once
  per document revision) and `CullPlan` (one coarse cull against a batch's union
  region, serving every brick in the dab) removed most of that. Measured on the
  deep-edit-list fixture, 8 bricks:

  | Items | Culling alone | A whole dab: cull + compile + evaluate |
  |---|---|---|
  | 193 | 0.019 ms | 0.140 ms |
  | 2 400 | 0.178 ms | 0.332 ms |
  | 10 000 | 0.926 ms | 0.934 ms |

  So the earlier claim that a 10,000-item sculpt is past the 4–8 ms interactive
  budget **on culling alone** is no longer true, and was wrong by about 16x once
  the index landed. It is recorded here rather than quietly rewritten, because
  the estimate was published and someone may have planned around it.

  What survives is the SLOPE: culling is still linear in item count across a 52x
  range, and it is still 99% of a dab's cost at 10 000 items. Removing it needs a
  spatial index over items, built once per edit and shared across the dab's
  bricks; the tape cache cannot help, because consecutive bricks want different
  cull regions. That is `add-item-spatial-index`, and by this measurement it runs
  out around 100,000 items rather than 10,000 — still worth doing, no longer
  urgent. `BM_DeepDocCullPlanned10000` and `BM_DeepDocRefillPlanned10000` gate
  the numbers above.
- **pyclay does not reach the brick cache** (added 0.24.0).
  `check_binding_parity.py` prints it as an outstanding follow-up on every run
  rather than filing it as an exemption, because that gate runs one way — pyclay
  to C — and a C-only addition cannot fail it. A Python binding wants a buffer
  protocol for the fp16 payloads and a numpy view over the request array.
- **SwiftPM consumption is verified; the app itself is not** (task 10.3).
  `tools/check_swift_smoke.sh all` builds a Swift program against the macOS
  slice and against the iOS simulator slice, running the latter *inside a
  booted simulator* via `simctl spawn`, and `swift run claycore-smoke` drives
  the same program through the package manifest an app would resolve. All 44
  checks pass on both, covering every primitive, all nine deformers, editing,
  undo, voxel sculpting with falloff brushes and the four verbs, meshing,
  validation, picking and the `.clayspace` round trip. What remains is opening
  the package in the real ClaySpace Xcode project and running on a device —
  the simulator is not a device, and only the app can prove the integration.
