## Why

**A document's voxel grid could not leave the interface thread except one
cell at a time.** Issue #658, from ClaySpaceDesktop#185 / #285: grid-to-field
froze the application (23k cells, 6.8 s on the interface thread). Moving the
conversion to a worker needed the grid off the document, and the ABI had no
call that copied a grid and no bulk read of its cells. A layer's grid is only
reachable as a BORROW (`clay_document_voxel_layer_by_id`), so the host:

1. walked the occupied bounding box with one `clay_voxel_get` per cell, empty
   cells included, because the box is all it can enumerate;
2. copied the palette entry by entry;
3. rebuilt the grid on the worker with `clay_voxel_set_many`;
4. converted that with `clay_item_volume_from_voxels`.

That snapshot cost 17–46 ms on the interface thread for 5k–100k cells, which
was the application's whole remaining interface-thread share. It captured the
active level only, dropped the sculpt layers, and cost the box rather than the
cells.

The threading contract was also unwritten. `clay_mesh_sculptor_create` states
which calls may run on a worker against a const document;
`clay_item_volume_from_voxels` and `clay_voxel_to_layer` said nothing.

## What the issue's suggested fix got wrong

The issue says `VoxelGrid` "is copyable by value, so a clone is
`new VoxelGrid(*g)` behind a handle." It is copyable, and that copy is the
defect. The member-wise copy also takes:

- `change_sink_` — a pointer into the DOCUMENT's undo journal (`History`'s
  `open_cells_`), live for the length of a voxel step;
- `pass_capture_` — a pointer to the document's open sculpt-layer pass
  record;
- `recording_` — "the next edit belongs to the open sculpt layer".

An edit to such a copy is appended to the source's history. The engine test
`a clone carries the cells and none of the source's recording channels` installs
a sink and a capture on a grid, clones it, edits the clone, and asserts the sink
and capture are untouched; run against `return VoxelGrid(*this)` it fails five
assertions — the sink is copied, the edit lands in it, and the capture records
the clone's rewrite of a cell of the source's pass.

Through the C ABI the sink and capture are installed only for the length of
one call (`VoxelStep`), so between calls they are null and a copy made at the
boundary would not carry them today. The recording flag IS live between calls,
and the C test catches it: a naive clone of a grid with an open sculpt layer
reports `clay_voxel_recording_sculpt_layer == 1`.

## What lands

```c
clay_result clay_voxel_grid_clone(const clay_voxel_grid* src, clay_voxel_grid** out_owned);
clay_result clay_voxel_get_occupied(const clay_voxel_grid* grid, int32_t* out_xyz,
                                    int32_t* out_index, size_t capacity, size_t* out_count);
```

- `VoxelGrid::clone()` — the member-wise copy, then the sink, the capture and
  the recording flag cleared, the change counter zeroed, the bounds cache cold,
  and every occupied chunk dirty (a grid nothing has drawn, as a grid read from
  a file is).
- `VoxelGrid::occupied_cells(level)` — every occupied cell of a level with its
  index, walking `material_chunk_keys` (stored and inherited) and sorted by z,
  then y, then x.
- THREADING notes in `clay.h` beside the clone, the bulk read,
  `clay_item_volume_from_voxels` and `clay_voxel_to_layer`, in the
  `clay_mesh_sculptor_create` wording, including the lazily mutating bounds
  cache (`grid.h` HAZARD note): two readers of a cold grid race.

ABI 0.122.0 -> 0.123.0.

## Measured

Apple M-series, `-O2`, in-process C++ through `libclay_shared` (so the box walk
below is ~10 ns a call — a host crossing an FFI per cell pays far more, which is
the 17–46 ms the issue reports). A borrowed document layer, a 3-cell-thick
sphere shell, median of 15:

| cells | box | box walk (`clay_voxel_get`) | `clay_voxel_grid_clone` | `clay_voxel_get_occupied` |
|---:|---:|---:|---:|---:|
| 4,184 | 25³ | 0.16 ms | 0.008 ms | 0.31 ms |
| 10,408 | 37³ | 0.51 ms | 0.008 ms | 0.49 ms |
| 30,880 | 61³ | 2.27 ms | 0.007 ms | 1.12 ms |
| 89,048 | 101³ | 10.96 ms | 0.074 ms | 4.12 ms |

The clone is the route off the interface thread: a copy of the material
chunks, independent of how many FFI calls a host's language costs.

## What building it found

- **The bulk read is mostly its sort.** With the sort disabled the 89k row
  reads 1.67 ms, against 4.12 ms with it. An ordered chunk walk (slab by slab,
  row groups by `ky`, runs by `kx`) would emit in order without sorting; it was
  not built, because it is five nested loops for a call that is not on the
  interface-thread path once a host clones, and the simpler design was
  preferred. The order is a promise either way.
- **The first walk was slower than the box walk.** It resolved each cell
  through `fmod_pos` as `ensure_bounds` does; reading a stored chunk's flat
  array in its own z, y, x layout took the 4k row from 0.65 ms to 0.31 ms. The
  ancestor read is kept for inherited chunks only.
- **`clay_item_volume_from_voxels` does not touch the bounds cache.**
  `to_field` sizes its box from `occupied_chunk_keys`, so the conversion is a
  pure read; the hazard belongs to `clay_voxel_bounds`, the raycasts and repair.
  The header says that rather than warning about the conversion.

## Not changed

- pyclay: nothing in the parity gate requires it (the gate checks that C
  reaches what pyclay reaches). A Python grid is a shared value already.
- `clay_voxel_to_layer` stays an interface-thread call: it adds a layer.
