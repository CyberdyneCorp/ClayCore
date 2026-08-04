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

## Backends

| Backend | Preset | Status | Capabilities |
|---|---|---|---|
| CPU | `cpu-only` | reference — always compiled in | everything; defines correctness |
| Metal | `metal` | tier 1 (the iPad app) | eval points/grid, raycast, hybrid meshing |
| CUDA | `cuda` | tier 2 | eval points/grid, raycast |
| OpenCL | `opencl` | tier 3, best-effort | eval points/grid; raycast and device meshing report `Unsupported` and fall back |

Backend availability changes speed, never results: every registered backend
is checked against the CPU scalar reference by the parity suite (1e-4
relative on distances, 1e-6 for the CPU batch path).

## Repository checks

```sh
python3 tools/check_layering.py        # module dependency rule
python3 tools/check_kernel_dialect.py  # kernel headers stay backend-portable
python3 tools/check_licenses.py        # permissive-license manifest gate
```

## License

MIT (see `LICENSE`). All dependencies are permissively licensed —
see `THIRD_PARTY_LICENSES.md`.
