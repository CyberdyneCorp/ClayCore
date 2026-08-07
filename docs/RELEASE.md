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
