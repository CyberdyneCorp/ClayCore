# Tasks: add-voxel-incremental-mesh

- [x] 1.1 Size the work against Part 1's result: re-measure the whole-grid re-mesh on the realistic blob at 0.05/0.03/0.02/0.015 on this branch's parent, and state in the proposal whether a whole-grid re-mesh now fits `INTERACTIVE_FRAME_SHARE_MS` — it does not, by 5.9x at the gate's 0.02 fixture size
- [x] 1.2 Dirty set per level on `VoxelGrid`, fed from `write_cell` only when the cell actually changed, including the neighbour chunk across a face and the chunk erased to empty
- [x] 1.3 `take_dirty_chunks()` drains it in a deterministic order; `dirty_chunk_count()` reports it without draining; a whole-grid mesh neither reads nor clears it
- [x] 1.4 Extract the mask build and the greedy merge into one per-window sweep so `mesh_greedy` and the regional mesh share it, with `mesh_greedy` byte-identical
- [x] 1.5 `mesh_greedy_chunks(keys, out_ranges)` over a per-chunk window, with `VoxelChunkMeshRange` per key in the order given and an empty range for a key holding no chunk
- [x] 1.6 C ABI: `clay_voxel_take_dirty_chunks` (capacity-in/count-out with a remainder, staged like the brick-cache drain) and `clay_voxel_mesh_chunks` with `clay_voxel_chunk_mesh_range`
- [x] 1.7 Seam test: mesh whole, mesh per chunk, assert the same set of exposed faces with the same colours and the same covered area, and that the per-chunk triangle count is >= the whole one
- [x] 1.8 Dirty-set tests: one per public mutation path, the neighbour-across-a-face case, the chunk-erased-to-empty case, the no-op write, and the per-level propagation case
- [x] 1.9 Drain tests: drain twice, write after a drain, a whole-grid mesh leaves the set alone
- [x] 1.10 Incremental-vs-whole equivalence: apply a stroke, patch the drained chunks over the previous per-chunk geometry, compare against a from-scratch whole-grid mesh
- [x] 1.11 `clay_voxel_mesh` output unchanged — the existing golden fixture test still passes unmodified
- [x] 1.12 C ABI tests: the drain's capacity/remainder loop, the NULL-buffer refusal, ranges-without-keys refusal, a stale key, and the range partition
- [x] 1.13 Measure the payoff: one dab on the 0.02 blob re-meshes 2.2 dirty chunks in 0.65 ms against 23.3 ms whole (35x); the per-chunk merge costs +3.7% triangles; dirty tracking costs the write path +0.8% on a million-cell rasterize
- [x] 1.14 Docs: `docs/05-claycore-library.md` §11 gains the incremental voxel display path
