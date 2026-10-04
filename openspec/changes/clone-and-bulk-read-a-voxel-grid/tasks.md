## 1. The gap

- [x] 1.1 `clay.h` had `clay_voxel_grid_create` / `_destroy`, a per-cell
      `clay_voxel_get`, and no clone or bulk read; a borrowed grid could only
      leave its document through a box walk
- [x] 1.2 The issue's `new VoxelGrid(*g)` copies `change_sink_`,
      `pass_capture_` and `recording_`; shown by running the engine test
      against `return VoxelGrid(*this)` (five failed assertions)

## 2. The change

- [x] 2.1 `VoxelGrid::clone()`: member-wise copy, sink and capture nulled,
      recording closed, change count 0, bounds cache cold, every occupied chunk
      dirty
- [x] 2.2 `VoxelGrid::occupied_cells(level)` over `material_chunk_keys`,
      stored and inherited chunks, sorted z, y, x
- [x] 2.3 `clay_voxel_grid_clone` (owned or borrowed source, owned result)
- [x] 2.4 `clay_voxel_get_occupied` with the size-query pattern and
      `CLAY_ERROR_BUFFER_TOO_SMALL` for a short capacity
- [x] 2.5 THREADING notes beside the clone, the bulk read,
      `clay_item_volume_from_voxels` and `clay_voxel_to_layer`, naming the
      bounds-cache race between two readers
- [x] 2.6 ABI 0.122.0 -> 0.123.0 in `CMakeLists.txt`, `clay.h`, `pyproject.toml`

## 3. Tests

- [x] 3.1 `test_c_voxel.cpp` "a clone of a borrowed layer is the whole grid
      and none of its document": two levels, non-default active level, a
      four-entry palette, an open sculpt layer; per-level counts, palette
      colours and cells match; editing the clone leaves the document's grid,
      its sculpt-layer record and its undo depth unchanged; destroying the clone
      succeeds and destroying the borrow is still refused
- [x] 3.2 `test_c_voxel.cpp` "the bulk read is the box walk, without the box":
      sparse grid and a partially refined level, element-for-element equal to a
      `clay_voxel_get` box walk and to `clay_voxel_occupied_count`; short buffer
      and null arguments
- [x] 3.3 `test_voxel.cpp` engine cases for the clone's channels and the
      bulk-read order, inherited cells included
- [x] 3.4 Mutations: naive copy (fails 3.1 on recording and 3.3 on sink,
      capture, recording, change count), skipping inherited chunks (fails 3.2
      and 3.3), dropping the sort (fails 3.2 and 3.3)
- [x] 3.5 `tools/check_c_abi.py` FFI-exercises the clone of a borrowed grid and
      the bulk read through ctypes

## 4. Docs

- [x] 4.1 `docs/05-claycore-library.md` C ABI section, `README.md` Voxel → SDF
