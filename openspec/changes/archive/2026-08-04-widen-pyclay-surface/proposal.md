# Proposal: widen the pyclay surface to the full C++ vocabulary

## Why

`pyclay` currently exposes documents, 12 primitives, the four core ops, blends, evaluation, meshing, and I/O — enough for the quickstart, but a strict subset of what claycore does. Five substantial capabilities are implemented, tested, and shipping in C++ yet unreachable from Python: sculpt strokes, the eight extended blend modes, the voxel engine, the surface-nets and dual-contouring meshers, and picking. That gap undercuts the reason the bindings exist (per the `python-bindings` spec): authoring the golden-scene corpus, procedural/batch generation, and ML dataset work all need strokes and voxels, and test authors cannot reach the meshers or picking they are meant to exercise.

Nothing new is being designed here — this is binding work over settled, already-verified C++.

## What Changes

- **Strokes**: `clay.Stroke(points=[(x, y, z, radius), ...], blend_k=…)` as a first-class primitive, appendable point-by-point, with per-stroke smoothing.
- **Extended blends**: the eight modes from the kernel vocabulary (`groove`, `tongue`, `pipe`, `engrave`, `emboss`, `inset`, `shell`, `replace`) as `clay.Op` members, including the second radius parameter groove/tongue take from item rounding.
- **Voxel engine**: `clay.VoxelGrid` — palette management, set/erase/paint singles and N³ brushes, box/line fills, mirrored edits, flood select, greedy meshing to a `Mesh`, `.clayspace` voxel-layer round trip, SDF↔voxel bridges (rasterize a document, sample the step field).
- **Alternate meshers**: `mesh(..., mesher="marching"|"nets"|"dual_contouring")` on `Document`, with dual contouring gated behind its experimental flag exactly as in C++.
- **Picking**: `doc.raycast(origin, direction)` returning hit position/normal/layer/item, `doc.snap_to_surface(point)`, `grid.raycast(...)` with cell + entry face, and selection bounds.
- **Batch-friendly numpy forms** wherever a loop would otherwise be needed: stroke points, voxel coordinate arrays, and ray batches accept `(N, k)` arrays.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `python-bindings`: the module's API surface requirement grows to cover strokes, extended ops, voxels, mesher selection, and picking; a new requirement fixes the numpy-batch contract for those APIs.

## Impact

- `bindings/python/pyclay_module.cpp` (the only C++ file that changes) and `bindings/python/tests/test_pyclay.py`.
- No change to the C++ library, the C ABI, or any other binding — this is additive Python surface over existing, tested behavior.
- Documentation: the pyclay section of `docs/05-claycore-library.md` and the README quickstart gain the new calls.
- Non-goals: deformers (the tape has no deformer opcodes yet — a Phase 3 item, not a bindings gap), the brick cache (an evaluation-internal detail with no standalone Python use case yet), and async/streaming evaluation.
