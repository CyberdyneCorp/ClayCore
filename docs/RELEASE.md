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

- **CUDA device parity has executed** (task 12.2, closed by
  `fix-cuda-arch-selection`). Validated on an RTX 5060 / driver 580 / nvcc
  12.0: the backend registers, `ctest --preset cuda` passes 4/4, and the parity
  suite runs 1115 assertions with the device visible against 479 with it hidden
  — the differential is what proves the extra assertions ran on the GPU rather
  than passing vacuously. pyclay at 1M points matches the CPU bit-for-bit,
  which is expected from the same single-source headers compiled `-fmad=false`.
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

  The consequence is that **three things are now manual and hardware-dependent**
  rather than gated, and all three must be run before a release that touches
  kernels:
  1. CUDA device parity (as below).
  2. The nvcc build of the backend, including its architecture auto-detection.
  3. That the OpenCL backend registers and passes parity on a real device.

  `python3 tools/release_check.py` run on a machine with those devices present
  covers all three, because it runs parity against every backend registered in
  that build.
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
