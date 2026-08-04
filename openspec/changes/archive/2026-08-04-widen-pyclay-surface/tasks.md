# Tasks: widen-pyclay-surface

## 1. Strokes

- [x] 1.1 `clay.Stroke(points=..., blend_k=...)` primitive: accepts a list of (x,y,z,r) tuples or an (N,4) float32 array; `add_point()` for incremental authoring
- [x] 1.2 Test: Python-authored stroke field matches the C++-authored equivalent; one tape item, not N

## 2. Extended blend ops

- [x] 2.1 Extend `clay.Op` with GROOVE/TONGUE/PIPE/ENGRAVE/EMBOSS/INSET/SHELL/REPLACE; expose the `rounding=` parameter groove/tongue read as channel half-width
- [x] 2.2 Test: each extended op evaluates identically to the C++ path and survives a .clayspace round trip

## 3. Voxel engine

- [x] 3.1 `clay.VoxelGrid`: palette add/get/set, get/set/erase/paint, N³ brushes, box/line fills, mirrored edits, flood select, occupancy + bounds queries
- [x] 3.2 Batch coordinate form: set/erase from an (N,3) int32 array
- [x] 3.3 `grid.mesh()` greedy meshing -> `clay.Mesh`; `grid.rasterize(doc, region)` and `grid.sample_step_field(points)` bridges
- [x] 3.4 Voxel layers in documents: `add_voxel_layer`, save/load through .clayspace
- [x] 3.5 Tests: edit ops, palette recolor, greedy-mesh triangle sanity, rasterize occupancy vs analytic volume, document round trip

## 4. Mesher selection

- [x] 4.1 `mesh(..., mesher="marching"|"nets"|"dual_contouring")`, dual contouring behind its experimental opt-in
- [x] 4.2 Test: nets produces strictly fewer triangles than marching on the same scene; DC requires the flag

## 5. Picking

- [x] 5.1 `doc.raycast(origin, dir)` -> hit(position, normal, layer, item); batch `(N,6)` form returning arrays
- [x] 5.2 `doc.snap_to_surface(points)` (batch), `grid.raycast(...)` with cell + entry face + adjacent cell
- [x] 5.3 `doc.selection_bounds(layer, nodes)` / `layer.bounds()`
- [x] 5.4 Tests: attribution on a two-item scene, snapped points satisfy |f| < tol, voxel face picking, batch forms agree with scalar forms

## 6. Docs & integration

- [x] 6.1 Update the pyclay section of docs/05-claycore-library.md and the README quickstart
- [x] 6.2 Full verification: python-ON ctest (incl. pytest), python-OFF ctest, wheel install quickstart, release checklist
