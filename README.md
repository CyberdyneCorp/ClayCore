# claycore

Portable, headless C++20 SDF + voxel engine library: single-source distance-field
kernels compiled into CPU, Metal, CUDA, and OpenCL backends; ordered-edit-list
scene semantics; sparse brick evaluation; watertight meshing; document and mesh
I/O; Python (`pyclay`) and C-ABI/Swift bindings.

claycore is the engine core of **ClaySpace** (iPad sculpting app) and stands
alone for tools, pipelines, CI, and research.

- Spec: `openspec/specs/` — the living requirements (eleven capabilities,
  from `sdf-kernels` to `build-packaging`).
- Math reference: `docs/01-sdf-math-foundations.md`.
- Architecture: `docs/05-claycore-library.md`.
- Releasing: `docs/RELEASE.md`.

## What ships today

| Area | Capability |
|---|---|
| Kernels | 25 exact + 4 bound 3D primitives, 9 exact 2D profiles, hard/quadratic/cubic/circular/chamfer blends plus 8 extended modes (groove, tongue, pipe, engrave, emboss, inset, shell, replace), transforms, mirrors, repetition, deformers (twist/bend/taper/displace) and spatial morphs, all reachable from a document, lifts, 33 easing curves, stroke chains, per-node exactness/Lipschitz tracking |
| Scene | Layers, ordered edit lists, nested groups, shared-content instancing, influence bounds, flat postfix tape with per-brick culling, invertible+serializable undo commands |
| Evaluation | CPU (reference + threaded batch), Metal, CUDA, OpenCL — one interface, runtime registry, tolerance-gated parity |
| Storage | Sparse fp16 narrow-band brick cache with dirty tracking, LOD mips, memory budget; palette-indexed colored voxel grids |
| Meshing | Marching tetrahedra (watertight + 2-manifold by construction), surface nets preview, flagged dual contouring, meshoptimizer decimation, validation, vertex colors/normals/UVs |
| Picking | Scene and brick raycast with layer/item attribution, surface snapping, voxel cell/face picking, selection bounds |
| I/O | `.clayspace` documents, OBJ+MTL, PLY, FBX (ufbx import + binary writer), glTF 2.0 GLB |
| Bindings | Stable C ABI (`clay.h`), SwiftPM xcframework, `pyclay` (nanobind, numpy-native: authoring incl. strokes and extended ops, voxels, evaluation, all three meshers, picking, I/O), `clay` CLI |

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
