# Tasks: speed-the-voxel-mesh-sweep

- [x] 1.1 Measure the defect on main: synthetic N-chunk grids (N = 1..64) and a rasterized blob at 0.05/0.03/0.02/0.015, recording total mesh time and ms per occupied chunk
- [x] 1.2 Hoist the chunk lookup out of the mask build: one `find` per (chunk, slice), the chunk's flat payload indexed by two constant strides, `cell_at` kept only for the neighbour probe that crosses a chunk boundary
- [x] 1.3 Keep empty space free: skip missing chunks and empty cells outright, on the merge's guarantee that it hands the mask back all-zero — state that guarantee where the merge makes it
- [x] 1.4 Keep it level-correct: the sweep reads the level it was handed, and every level of a multi-level grid is pinned
- [x] 1.5 Golden fixture test: single cell, negative coordinates, cells on every chunk seam, solid blocks straddling a seam, a rasterized blob in both the positive octant and across the origin, all levels of a multi-level grid, 64 sparse chunks, and the empty grid — vertex count, index count and a hash of every buffer
- [x] 1.6 Cross-library fingerprint: the same program run against the previous `libclay_shared.so` and the new one over every fixture, diffed
- [x] 1.7 Re-measure on the branch, same machine, back to back, and record the ratio
