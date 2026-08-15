# Tasks: batch-mesh-attribute-taps

## 1. Backend

- [x] 1.1 `PointBatchQuery` + `Backend::eval_points_batch` default: many point runs, each with its own tape; default loops `eval_points` per run so every backend answers identically
- [x] 1.2 CPU override: one `parallel_for` over the flattened batch, chunks spanning run boundaries walk the runs they cover; per-point results identical to the default by construction

## 2. Mesher

- [x] 2.1 `apply_brick_attributes` compiles one culled tape per vertex group (unchanged, shared by normals and colors) and evaluates every group through a single `eval_points_batch` call; serial fallback only when no CPU backend is registered

## 3. Tests and gates

- [x] 3.1 Parity, bitwise: CPU override vs the Backend default vs per-run `eval_points`, memcmp on distances/gradients/colors over culled tapes with empty, one-point and multi-chunk runs (`tests/unit/test_points_batch.cpp`)
- [x] 3.2 Input validation: empty batch Ok; null tapes/offsets/points/distances, a null tape entry, and decreasing offsets refused
- [x] 3.3 Regression, byte identity: `mesh_bricks` with gradient normals + colors memcmp-equal to the serial per-group evaluation it replaced, on the gnarly corpus
- [x] 3.4 Scaling gate: `BM_MeshBricksGradDenseDoc` vs `BM_DabRefillDenseDoc` — dense re-mesh attributes held to a bounded multiple of refilling the same bricks; `check_bench.py` gates the ratio at 10x

## 4. Docs

- [x] 4.1 `docs/05-claycore-library.md`: backend interface lists `eval_points_batch`; meshing attributes describe the batched evaluation
