# Tasks: triangles straight to cells

## 1. The BVH query the colour needs

- [x] 1.1 `Bvh::closest` — point, source triangle, barycentrics; ties resolve to
      the triangle found first so the answer does not depend on traversal order.
- [x] 1.2 `unsigned_distance` reimplemented on top of it, so one traversal
      serves both and they cannot drift. Results unchanged, asserted by the
      existing distance and winding tests.

## 2. The bridge

- [x] 2.1 `VoxelGrid::rasterize_mesh(mesh)` — region defaults to the mesh's own
      bounds, which a mesh always has and a document may not.
- [x] 2.2 `VoxelGrid::rasterize_mesh(mesh, region)` — the bounded form.
- [x] 2.3 Membership by generalized winding number at the cell centre.
- [x] 2.4 Colour interpolated at the closest point on the nearest triangle and
      quantised to the palette; one neutral entry for a mesh without colours.
- [x] 2.5 The header carries what the sampling preserves and what it costs, and
      says this is occupancy sampling and not retopology.

## 3. Tests

- [x] 3.1 The solid is filled, its volume within a half-cell of the box's.
- [x] 3.2 The region defaults to the mesh's bounds; an explicit one bounds the
      work and says nothing about the rest.
- [x] 3.3 A holed mesh does not flip a half-space.
- [x] 3.4 Vertex colours reach the palette; a colourless mesh takes one entry.
- [x] 3.5 One sampling against two: the direct path keeps at least as much of a
      thin slab, and agrees within a cell of surface on a thick one.
- [x] 3.6 Empty mesh, empty region and all-bad indices each change nothing and
      are not errors.
- [x] 3.7 `change_count` moves on the first rasterize and not on the second.

## 4. Bindings

- [x] 4.1 `clay_voxel_rasterize_mesh`, region optional, refusals before the grid
      is touched.
- [x] 4.2 `VoxelGrid.rasterize_mesh` in pyclay, GIL released.
- [x] 4.3 C tests and Python tests, including the refusals both sides share.
- [x] 4.4 `tools/check_binding_parity.py` passes.

## 5. Docs and the example

- [x] 5.1 `examples/48_mesh_to_voxels.py` with committed renders: the direct
      path against the detour on a thick model, a thin fin, and colour; a holed
      model; and voxel verbs applied to the import.
- [x] 5.2 README's conversion section gains the mesh → voxel direction.
- [x] 5.3 `docs/07` reachability table row.
- [x] 5.4 `openspec/ROADMAP.md` row.
