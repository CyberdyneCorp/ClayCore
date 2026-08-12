# Tasks: cull-brick-mesh-attributes

## 1. Mesher (issue #73)

- [x] 1.1 `mesh_bricks` takes the `scene::Document` for attributes instead of a pre-compiled tape — a whole tape is exactly what cannot be culled per brick
- [x] 1.2 Attribute pass groups vertices by the brick owning their position and evaluates each group against a tape culled to that brick's `cull_region`, dilated additionally by `gradient_eps` so the tetrahedron taps stay inside the culled zone
- [x] 1.3 `clay_brick_cache_mesh` passes the document through instead of compiling the whole tape; the no-document and face-normal paths are unchanged

## 2. Docs

- [x] 2.1 `clay.h`: CLAY_NORMAL_GRADIENT states that gradient cost follows the bricks being meshed, not the document, and why quality is unchanged
- [x] 2.2 `include/clay/mesh/marching.h`: the same contract on `mesh_bricks`

## 3. Tests and gates

- [x] 3.1 Regression, correctness: gradient normals and colours for a fixed brick set in a document with 200 far-away nodes equal those of the same bricks in a document holding only the nearby nodes, and equal `clay_eval_gradients` over the full document at the same vertices
- [x] 3.2 Regression, scaling: `BM_MeshBricksGradFreshDoc` / `BM_MeshBricksGradGrownDoc` mesh the same 80-brick pole subset against 1-node and 193-node documents; `check_bench.py` gates the grown/fresh ratio at 3x
