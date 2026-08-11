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
   --all --strict`, benchmark floors, and a real `pip install .` quickstart in
   a throwaway venv.
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

  `python3 tools/release_check.py` run on a machine with those devices present
  covers all four, because it runs parity against every backend registered in
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
  20× at 128³. So the two workloads want opposite backends: keep
  `clay_brick_cache_eval_requests` on `"cpu"`, and send preview grids of 32³ and
  up to `"metal"`. The header's advice to fan out over requests one brick per
  worker *is* now measurement: it takes a 216-brick fill from 24.7 ms to 8.2 ms
  on twelve workers, a 3.0x speedup. Metal and the CPU agree exactly over a full
  fill (max abs difference 0.0), so this is a speed result, not a parity one.

  What is still untested is a *tablet*. An M2 Max has twelve cores, active
  cooling and a 34 GB unified pool; an iPad has fewer cores, a hard thermal
  ceiling and a memory budget that kills apps rather than swapping. The fan-out
  gain assumes cores that stay at clock, and the 16³ crossover is the number
  most likely to move on a GPU with a different dispatch cost. Re-measure both
  on the target iPad before wiring up the split.
- **A brush dab's brick COUNT is flat, but its cost is not** (added 0.24.0).
  The count claim holds as designed and as tested: holding density constant
  while the document grows from 100 to 2400 items, a dab keeps dirtying 22–24
  bricks. Its *time* does not stay flat — 2.6 ms to 8.8 ms over that same range,
  because `clay_brick_cache_eval_requests` compiles a culled tape per brick and
  that compile walks every node in the document, measured at ~64 ns per item per
  brick across a 24x range. A dab pays it ~24 times, so at 2400 items culling
  alone is ~3.6 ms before a sample is evaluated, and the same rate puts a
  10,000-item sculpt past the 4–8 ms interactive budget on culling alone.
  Fanning out buys back about a factor of two, which lowers the constant without
  changing the slope. Removing the slope needs a spatial index over items, built
  once per edit and shared across the dab's bricks; the tape cache cannot help,
  because consecutive bricks want different cull regions.
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
