# Proposal: claycore — Portable C++20 SDF + Voxel Engine Library

## Why

ClaySpace (the iPad app, `../ClaySpace`) needs a single source of truth for all SDF/voxel mathematics, scene semantics, evaluation, meshing, and file I/O — one that also stands alone as a reusable, headless engine for CI, Python tooling, pipelines, and future ports. Without it, every distance function and blend gets re-implemented per platform (Swift, MSL, CUDA, Python) and the copies drift; with it, one C++20 kernel header compiles into every CPU and GPU backend and a document evaluates identically everywhere. The math is already catalogued (`docs/01-sdf-math-foundations.md`) and the architecture decided in ClaySpace's `add-clayspace-v1` change (Dreams-style brick cache, ordered edit lists, rigid blends); this change turns that into the library's own spec.

## What Changes

- New pure C++20 library **claycore**: headless by construction — no UI, no windowing, no Apple framework dependencies in the core; builds and runs its full test suite on macOS, Linux, and Windows.
- **Single-source kernels**: every primitive SDF, blend, operator, transform, repetition, deformer, and lift written once in a restricted C++ "kernel dialect" header and compiled into CPU (scalar reference + SIMD), Metal (MSL), CUDA, and OpenCL from the same source. CPU scalar is the correctness reference; all backends must match within documented tolerances.
- **Scene model** owned by the library: layers (voxel | SDF), ordered edit lists, groups, instancing, per-item influence bounds, blend locality guarantee, serializable undo command vocabulary, and the edit-list → flat postfix tape compiler (fixed tape-interpreter kernel per backend — no per-edit shader recompiles).
- **Sparse brick cache**: 8³/16³ fp16 narrow-band bricks, dirty tracking, per-brick tape culling, LOD mips.
- **Colored voxel engine**: palette-indexed chunked grids, set/erase/paint/fill/mirror/flood ops, greedy meshing, voxel↔SDF bridges.
- **Meshing**: marching cubes (watertight/manifold guarantee, golden CPU reference), surface nets preview, dual contouring behind a flag, color-aware decimation (meshoptimizer), mesh validation (the primitive behind export gates).
- **Picking**: ray ↔ scene raycast with hit attribution, surface snapping, build-plane/grid resolution, bounds queries — CPU-side, Pencil-latency-critical.
- **File I/O**: `.clayspace` binary chunked document format (versioned, backward-open/forward-refuse), OBJ+MTL, FBX (ufbx import, minimal binary writer), PLY, glTF/GLB writer. USDZ explicitly excluded (app shell owns it).
- **Acceleration backends**: CPU (always compiled in), Metal via metal-cpp (tier 1 — the iPad app), CUDA via NVRTC/nvcc (tier 2), OpenCL 3.0 (tier 3, best-effort). Runtime-registered; availability never changes results, only speed — enforced by a parity suite.
- **Python bindings** (`pyclay`): nanobind, numpy-native, shipped as wheels; used to author the golden-scene test corpus, exercise the library, and serve procedural/batch/ML workflows.
- **C ABI** (`clay.h`): flat, stable, versioned — the boundary Swift (and any FFI) links against; SwiftPM wrapper target for the Xcode app.
- **clay-cli**: eval/mesh/convert/validate from the command line — CI's workhorse.
- Full test pyramid: kernel units vs. reference values, property tests (Lipschitz bounds, blend rigidity, locality bit-identity), backend parity, golden-scene meshing, I/O round-trip + fuzz, performance regression gates.

## Capabilities

### New Capabilities

- `sdf-kernels`: The kernel dialect and the complete SDF vocabulary — 3D/2D primitives, booleans and smooth/chamfer/extended blends, transforms, symmetry, repetition, deformers, lifts, easing curves, stroke items, field utilities (normals, AO, shadows, sphere tracing) — with per-node exactness/Lipschitz tracking.
- `scene-model`: Document tree (layers, ordered edit lists, groups, instancing), influence bounds and blend locality, tape compilation with per-brick culling, serializable undo command vocabulary.
- `evaluation-backends`: The backend interface (`eval_points`, `eval_bricks`, `raycast`, `mesh`, capability flags), runtime registry, CPU reference + SIMD, Metal, CUDA, OpenCL hosts, and the cross-backend parity contract.
- `brick-cache`: Sparse narrow-band brick storage, dirty tracking, incremental re-evaluation, LOD mips, deterministic memory ceilings.
- `voxel-engine`: Palette-indexed chunked voxel grids, editing ops, mirror, flood select, greedy meshing, voxel↔SDF bridges.
- `meshing`: Marching cubes, surface nets, dual contouring, decimation, mesh validation, vertex attributes (colors, normals, optional UVs).
- `picking`: Raycast with attribution, surface snapping/closest-point, build-plane and voxel-face resolution, bounds/frustum utilities.
- `file-io`: `.clayspace` document format and OBJ/MTL, FBX, PLY, glTF/GLB interchange with import guardrails.
- `python-bindings`: The `pyclay` nanobind module, numpy-native API, wheel packaging.
- `c-abi`: The flat versioned C API (`clay.h`), error-code discipline, opaque handles, SwiftPM consumption.
- `build-packaging`: CMake presets, SwiftPM wrapper, wheels, clay-cli, dependency policy (permissive licenses only), test pyramid and CI gates.

### Modified Capabilities

_None — greenfield repo, no existing specs._

## Impact

- New repository layout: `include/clay/` (kernel, math, scene, eval, brick, voxel, mesh, pick, io), `src/`, `backends/{cpu,metal,cuda,opencl}`, `bindings/{c,python}`, `tests/`, `tools/`.
- Third-party dependencies (all permissive): ufbx, meshoptimizer, metal-cpp, nanobind, xsimd, cgltf/tinyply (optional), doctest/Catch2 + google-benchmark. assimp used in CI only as an independent export validator. No Boost, no GPL/LGPL, no exceptions across the ABI.
- ClaySpace (`../ClaySpace`) becomes the first consumer via the C ABI / SwiftPM wrapper; its `add-clayspace-v1` change depends on this library's Phase 1 deliverables.
- Reference libraries for ideas/code reuse: CyberRemesherAndUV, CyberCadKernel, SymPP, NumPP (github.com/CyberdyneCorp).
- Delivery is phased (see `design.md` §Phasing): Phase 1 = the app's dependency set (kernels, scene/tape, bricks, CPU+Metal, MC meshing, OBJ/FBX/PLY + document I/O, C ABI, clay-cli); Phase 2 = pyclay wheels, extended blends, surface nets/DC hardening, glTF; Phase 3 = CUDA; Phase 4 = OpenCL. Later phases may be split into follow-up OpenSpec changes at implementation time, but the requirements are specified here once.
- Non-goals (scope guardrail): rendering/UI of any kind, USDZ export, Vulkan compute (noted as OpenCL's likely successor, not specified), SYCL, cloud services, interval-arithmetic tape shortening (roadmap note only).
