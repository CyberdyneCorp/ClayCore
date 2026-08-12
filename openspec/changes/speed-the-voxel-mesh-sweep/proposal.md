# Proposal: speed the voxel mesh sweep

## Why

`VoxelGrid::mesh_greedy` costs a flat ~4 ms per occupied chunk however empty
that chunk is (#86, part 1). A chunk holding one voxel costs the same as a
full one, so a realistic sculpt at the gate's 0.02 cell size takes hundreds of
milliseconds to display against 0.026 ms to edit.

The slab grouping already fixed the bounding-box blowup, so the cost is not the
window size — it is the probe. The mask build reads every cell of the window
through `cell_at`, which is a hash plus an `unordered_map::find`. Six
directions x 32 slices x a 32x32 window is ~200K map lookups per chunk, and
that is the whole 4 ms.

The existing cost requirement says sparse operations cost the material and not
the bounding box. It is silent on the per-cell constant, which is what actually
made the sweep unaffordable, so a conforming implementation could — and did —
pay a hash per cell and still satisfy it.

## What

State the missing half of the cost model and make the sweep meet it.

- The exposure mask build resolves the chunk once per (chunk, slice) instead of
  once per cell. The mask window is chunk-aligned by construction, so within a
  slice every cell of a chunk column shares one key; the flat `kChunkDim^3`
  payload is then indexed directly with two constant strides.
- Cells in no chunk, and empty cells inside a chunk, write nothing. The greedy
  merge hands the mask back all-zero, so absence is already the mask's resting
  value and empty space costs no work rather than a lookup returning zero.
- The one probe that can leave the chunk — the neighbour across the face, on the
  slice sitting on the chunk's own boundary — still goes through `cell_at`.

## What it does not touch

- **The output.** Byte-identical: same quads, same order, same vertex and index
  buffers. This is a refactor behind an unchanged result, gated by a golden
  fixture test and by a cross-library fingerprint diff against the previous
  build.
- **The public API.** No new entry point, no signature change, no ABI move.
- **Dirty tracking and regional meshing.** Part 2 of #86, and a separate change:
  it adds API and changes what a caller can ask for.

## Impact

`voxel-engine` — the sparse-cost requirement gains the per-cell constant.
No `c-abi` delta; `clay_voxel_mesh` is unchanged in signature and in output.
Measured on Linux desktop (which `docs/RELEASE.md` forbids comparing against
the device baseline; the ratio transfers, the absolute numbers do not): the
synthetic 64-chunk grid goes 263.8 ms to 10.0 ms, and the 0.02 blob 395.8 ms to
17.6 ms — ~26x and ~22x, with per-chunk cost dropping from ~4.1 ms to ~0.16 ms.
