# Tasks: smooth voxel display

## 1. The mesher

- [x] 1.1 An occupancy sampler over a level, at voxel centres, negative inside.
- [x] 1.2 `VoxelGrid::mesh_smooth(level, options)` over `mesh_lattice_nets`.
- [x] 1.3 Per-vertex colour: the average of the palette colours of the occupied
      cells adjacent to the vertex.
- [x] 1.4 An optional occupancy blur, off by default, documented as the setting
      that can erase a thin feature.

## 2. The C ABI

- [x] 2.1 `clay_voxel_mesh_smooth`, taking the smoothing setting explicitly.
- [x] 2.2 `clay_voxel_mesh` unchanged and byte-identical.

## 3. Tests

- [x] 3.1 A solid box rounds; the same grid through `mesh_greedy` is unchanged.
- [x] 3.2 A lone voxel survives at the default setting.
- [x] 3.3 Colour blends between two palette regions, and no vertex carries a
      colour absent from the palette.
- [x] 3.4 `mesh_greedy` output is byte-identical to before this change, over
      the existing golden fixtures.
- [x] 3.5 An empty grid meshes to an empty mesh, both ways.

## 4. The picture

- [x] 4.1 An example rendering the same sculpt blocky and smooth side by side,
      committed to the gallery — the gallery is how this issue was found and
      it is how the fix is checked.

## 5. Measurement

- [x] 5.1 A benchmark against `mesh_greedy` on the same grid, so the cost of
      the smooth path is a number rather than an impression.
- [x] 5.2 Say plainly in the docs whether the whole-grid smooth mesh fits an
      interactive frame, and record that per-chunk smooth meshing (the
      incremental path) is NOT in this change.
