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
   ABI hygiene + ctypes FFI, `openspec validate --all --strict`, benchmark
   floors, and a real `pip install .` quickstart in a throwaway venv.
3. On a minor/patch release, read the `clay.h` diff for symbol and
   struct-layout breaks (the ABI gate checks hygiene, not history). Below 1.0
   a break is allowed on a minor bump under SemVer's 0.x rule, but it is never
   silent: say so in the release notes, and make the library reject the older
   layout instead of misreading it. **0.2.0 is such a release** — the leading
   `struct_size` shifted every field of `clay_item_desc` and
   `clay_mesh_params`, so ABI 0.1.0 binaries must recompile.

## Tagging

```sh
git tag -a v1.0.0 -m "claycore v1.0.0"
git push origin v1.0.0
```

`.github/workflows/release.yml` then re-runs the checklist on Linux, builds
wheels for macOS/Linux/Windows via cibuildwheel, builds the
`claycore.xcframework` with its SwiftPM checksum, and opens a **draft**
GitHub release with everything attached. Drafts are deliberate: review the
artifacts before publishing.

## Open items before v1.0

Tracked honestly rather than assumed done:

- **CUDA device parity has never executed.** The backend compiles under nvcc
  in CI and the kernel headers pass the CUDA dialect profile, but no NVIDIA
  device exists on the dev machine or on GitHub runners. Build the `cuda`
  preset on CUDA hardware and run `ctest --preset cuda`; the parity suite and
  pyclay's `test_every_registered_backend_matches_cpu` pick the backend up
  automatically. (openspec task 12.2)
- **CUDA-enabled wheels are not shipped** (task 12.3). Wheels currently carry
  the CPU backend, which per the parity contract changes speed, not results.
  Shipping CUDA wheels needs a CUDA build host in the wheel matrix.
- **Blender-headless FBX validation** runs only as a release-time manual
  check; per-push CI validates exports with assimp instead (task 9.3).
- **In-app SwiftPM verification** awaits the ClaySpace Xcode project; the
  xcframework slices build and a Swift smoke consumes the module (task 10.3).
