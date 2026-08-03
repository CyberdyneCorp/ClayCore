# Design: claycore

## Context

ClaySpace (`../ClaySpace`, OpenSpec change `add-clayspace-v1`) specifies an iPad voxel + SDF sculpting app whose entire math/scene/meshing/I-O core was decided to live in a portable C++20 library. The math inventory is complete (`docs/01-sdf-math-foundations.md`); the library's shape is described in `docs/05-claycore-library.md`. This repo is greenfield (LICENSE only). Consumers in priority order: the iPad app (C ABI/SwiftPM, Metal), CI (headless Mac/Linux/Windows), Python users (`pyclay`), future ports.

Reference/donor libraries from the same org, to mine for ideas or reuse where licenses and quality fit: [CyberRemesherAndUV](https://github.com/CyberdyneCorp/CyberRemesherAndUV) (remeshing/UV — relevant to decimation/UV utilities), [CyberCadKernel](https://github.com/CyberdyneCorp/CyberCadKernel) (geometry kernel patterns, robust predicates), [SymPP](https://github.com/CyberdyneCorp/SymPP) and [NumPP](https://github.com/CyberdyneCorp/NumPP) (symbolic/numeric C++ utilities — candidate for test-side reference math, not for the kernel dialect, which must stay dependency-free).

## Goals / Non-Goals

**Goals:**
- One implementation of every distance function/operator, compiled into CPU + Metal + CUDA + OpenCL from the same headers; CPU scalar defines correctness.
- Scene semantics (ordered edit lists, rigid blends, influence bounds, locality) owned here so every consumer evaluates documents identically.
- Headless core: builds and fully tests on macOS/Linux/Windows with zero UI/platform dependencies.
- Python (`pyclay`) as both a product surface and the test-authoring harness; Swift consumption via a stable C ABI + SwiftPM wrapper.
- Deterministic memory ceilings and allocation discipline suitable for mobile.
- Permissive licensing throughout (MIT/Apache-2 library; MIT/BSD/zlib deps).

**Non-Goals:**
- Rendering, UI, windowing, input (the app owns those).
- USDZ (Apple Model I/O in the app shell owns it; we expose buffers).
- Vulkan compute / SYCL backends (interface leaves room; not specified).
- Interval-arithmetic tape shortening (Keeter MPR) — roadmap note, not in scope.
- Bit-exact cross-GPU results (impossible under FMA/fast-math; tolerance-based parity instead).

## Decisions

- **D1 — Kernel dialect over per-backend ports.** All math in restricted C++ headers (`no virtual/exceptions/alloc/recursion`), with `shim.h` mapping types/qualifiers per backend macro. MSL is C++14-based, CUDA is C++, OpenCL gets the C-compatible subset via macros. Rationale: the alternative (hand-ported Swift/MSL/CUDA copies) is exactly the drift this library exists to kill.
- **D2 — Fixed tape interpreter, not shader codegen.** Scenes compile to a flat postfix tape (opcodes + param blocks, transforms pre-inverted); each backend ships one interpreter kernel. No per-edit recompiles → instant parameter edits (the mzschwartz5 lesson); leaves the door open to MPR-style tape shortening later.
- **D3 — CPU scalar is the reference; parity is tolerance-based.** Default 1e-4 relative on distances, per-kernel overrides documented. GPU availability changes speed, never results (within tolerance) — a release gate, not an aspiration.
- **D4 — Rigid blends only in the core vocabulary.** Finite-support smins (quadratic default, cubic, circular, chamfer) make influence bounds real, which makes brick culling, incremental re-eval, and the locality bit-identity guarantee provable and testable.
- **D5 — Exactness/Lipschitz tracked per node.** `exact | bound | Lipschitz(L)` propagates through the tree (01 §2.7); consumers get a safe step scale instead of assuming |∇f| = 1.
- **D6 — Dreams-style sparse brick cache.** 8³/16³ fp16 narrow band (±3 voxels), dirty sets from influence bounds, per-brick culled tapes, LOD mips. Plain-data async requests; the consumer owns threading.
- **D7 — One undo/serialization command vocabulary.** Every mutation is a serializable command with an inverse; the document file is (logically) a command log plus compact state chunks. One vocabulary → tiny undo steps, stroke coalescing, and Python/CI document authoring for free.
- **D8 — Meshing: MC default with watertight guarantee; DC behind a flag.** Asymptotic-decider MC is the golden reference; GPU meshers match topology invariants, not vertices. Surface nets for previews. DC (manifold variant) ships flagged until hardened post-v1. Decimation via meshoptimizer, color-aware.
- **D9 — C ABI as the app boundary.** Flat `clay.h` (opaque handles, error codes, caller buffers, size-query pattern) + SwiftPM wrapper. Swift-C++ interop may be layered on later but the ABI is the contract. `std::expected`-style internally; nothing throws across the boundary.
- **D10 — nanobind + scikit-build-core + cibuildwheel** for `pyclay`. numpy-native, GIL released during evaluation. Python authors the golden corpus — tests are a consumer, which keeps the bindings honest.
- **D11 — I/O choices.** ufbx for FBX import (MIT, single-file, battle-tested); minimal custom binary FBX writer (exporting is far smaller than importing); dependency-free OBJ/MTL; PLY both ways; glTF via cgltf or custom. assimp + headless Blender validate exports in CI only.
- **D12 — Backend tiers.** Metal is tier 1 (the app), CUDA tier 2 (pipeline/ML batch), OpenCL tier 3 best-effort with Vulkan compute as the anticipated successor behind the same `Backend` interface.

## Risks / Trade-offs

- **Kernel dialect is a straitjacket** — the OpenCL C-compatible subset in particular restricts idioms (no references in some paths, address-space qualifiers). Mitigation: dialect enforced by CI compile checks from day one; OpenCL is tier-3 so it constrains but never blocks.
- **Watertight guarantee vs GPU meshing** — GPU MC with shared vertex welding is fiddly; we guarantee topology invariants rather than bit-identical vertices, and keep CPU MC as the export path of record if a backend can't meet the gate.
- **fp16 narrow band precision** — ±3-voxel band in fp16 can quantize thin features; brick resolution and band width are per-document parameters, and the parity/golden tests run at the app's shipping resolutions.
- **Minimal FBX writer compatibility** — binary FBX is under-documented; mitigated by CI round-trips through assimp and Blender headless for every golden export.
- **Scope breadth** — four backends + bindings is a lot; phasing (below) keeps the app's dependency set (Phase 1) first, and later phases can be re-proposed as separate OpenSpec changes if priorities shift.

## Phasing

1. **Phase 1 (app dependency set):** kernel headers (primitives, core ops, smooth/chamfer, transforms, repetition, strokes), scene/tape/undo, brick cache, CPU + Metal backends, MC meshing + decimation + validation, OBJ/FBX/PLY + document I/O, C ABI + SwiftPM, clay-cli, full test pyramid.
2. **Phase 2:** pyclay wheels; extended blend vocabulary; surface nets; DC hardening; glTF writer.
3. **Phase 3:** CUDA backend; deformer family surfaced; (roadmap: interval-arithmetic tape culling).
4. **Phase 4:** OpenCL backend (or Vulkan-compute successor evaluation).
