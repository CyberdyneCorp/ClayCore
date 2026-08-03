# claycore

Portable, headless C++20 SDF + voxel engine library: single-source distance-field
kernels compiled into CPU, Metal, CUDA, and OpenCL backends; ordered-edit-list
scene semantics; sparse brick evaluation; watertight meshing; document and mesh
I/O; Python (`pyclay`) and C-ABI/Swift bindings.

claycore is the engine core of **ClaySpace** (iPad sculpting app) and stands
alone for tools, pipelines, CI, and research.

- Spec: `openspec/` (active change: `add-claycore-v1`) — the source of truth
  for requirements and the implementation roadmap (`tasks.md`).
- Math reference: `docs/01-sdf-math-foundations.md`.
- Architecture: `docs/05-claycore-library.md` and
  `openspec/changes/add-claycore-v1/design.md`.

## Build

Requires CMake ≥ 3.24 and a C++20 compiler.

```sh
cmake --preset cpu-only     # any platform; +metal / +cuda / +opencl presets exist
cmake --build --preset cpu-only
ctest --preset cpu-only
```

## Repository checks

```sh
python3 tools/check_layering.py        # module dependency rule
python3 tools/check_kernel_dialect.py  # kernel headers stay backend-portable
python3 tools/check_licenses.py        # permissive-license manifest gate
```

## License

MIT (see `LICENSE`). All dependencies are permissively licensed —
see `THIRD_PARTY_LICENSES.md`.
