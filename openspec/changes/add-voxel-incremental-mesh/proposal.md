# Proposal: dirty-chunk tracking and a regional voxel mesh

## Why

`clay_voxel_mesh` is the whole grid, always. There is no way to ask for the
part that changed, so a host that wants to see its sculpt pays for every
occupied chunk on every display call while a dab dirties a handful (#86,
part 2).

Part 1 (`speed-the-voxel-mesh-sweep`) took the per-chunk cost from ~4.1 ms to
~0.16 ms by hoisting the chunk lookup out of the mask build. That was worth
~25x and it moved the wall, but it did not remove the O(document) term from
an interactive loop. Measured on this branch's parent, Linux desktop — which
`docs/RELEASE.md` forbids comparing against the device baseline; the ratio
transfers, the absolute numbers do not:

| blob cell size | occupied cells | occupied chunks | whole-grid re-mesh | vs the 4.17 ms share |
|---|---|---|---|---|
| 0.05 | 62K | 8 | 2.38 ms | 0.6x — fits |
| 0.03 | 289K | 35 | 10.9 ms | 2.6x |
| **0.02** (the gate's fixture size) | 975K | 81 | **24.5 ms** | **5.9x** |
| 0.015 | 2.31M | 147 | 47.1 ms | 11.3x |

`INTERACTIVE_FRAME_SHARE_MS` is 4.17 ms and the host still has to draw in it.
So the issue's own bar for "Part 2 becomes an optimisation rather than a
necessity" — a realistic sculpt re-meshing whole inside the frame — is missed
by 5.9x at the size the device gate itself fixtures, and by 11.3x one step
finer. **Part 2 stays a necessity.** It is also now a plausible one-frame
path rather than a hope, and it landed inside it: a dab dirties ~2 of the 81
chunks and re-meshes in 0.65 ms.

Cost is still linear in occupied chunks and still charged on every display
call. That is the shape to remove, not the constant.

## What

The brick cache already solved this shape on the SDF side — `mark_dirty` ->
`take_dirty` -> `mesh` with per-key ranges. This is the same vocabulary on the
voxel side, and deliberately not a parallel one.

- **A dirty set on the grid**, per level, fed from `write_cell` — the single
  choke point every public mutation funnels through, so no verb can forget to
  report. A write on a chunk FACE also dirties the neighbour whose exposure it
  changes, because the mask build probes across that seam. A chunk that
  reaches zero occupancy is erased, and it is reported dirty on the way out or
  its quads are never removed.
- **`VoxelGrid::take_dirty_chunks()`** drains it, as
  `clay_brick_cache_take_dirty` does, so a host that skipped a frame coalesces
  rather than replaying.
- **`VoxelGrid::mesh_greedy_chunks(keys, out_ranges)`** meshes only the named
  chunks and reports what each contributed, so a host patches GPU sub-ranges
  instead of rebuilding the buffer. `mesh::BrickMeshRange` is the precedent.
- **C ABI:** `clay_voxel_mesh_chunks` and `clay_voxel_take_dirty_chunks`
  beside `clay_voxel_mesh`, which keeps meaning "the whole grid" and keeps its
  current output byte for byte.

### The seam argument

Greedy quads are axis-aligned and exact, and the exposure test reads the
neighbour cell wherever it lives — including across a chunk seam. So clamping
the merge to a chunk boundary produces MORE, SMALLER quads over the IDENTICAL
surface: the same set of exposed faces, the same covered area, the same
colours, never a crack.

Per-chunk voxel meshing therefore needs no straddler attribution, unlike
`mesh_bricks` (#66), where a marching-cubes cell straddles the brick boundary
and its triangles belong to a cell no request named. A voxel face belongs to
exactly one cell, which belongs to exactly one chunk. That is the reason this
part is tractable at all, and it is why the ranges here PARTITION the mesh
with no shared vertices, where the brick ranges do not.

The cost is triangle count at chunk boundaries and nothing else. Measured on
the 0.02 blob: 62,554 triangles whole against 64,852 per-chunk, +3.7%.
`clay_voxel_mesh` stays whole-grid, so export keeps the tighter merge.

## What it does not touch

- **`clay_voxel_mesh` and `VoxelGrid::mesh_greedy`.** Same signature, same
  sweep, byte-identical vertex and index buffers. Gated by the golden fixture
  test Part 1 added.
- **The mutation verbs.** Every one of them already writes through
  `write_cell`; none of them changes shape, and none learns about dirtiness.
- **Serialization.** The dirty set is session state, not document state: it
  says what a HOST has not drawn yet, which no reader of a file can know. A
  deserialized grid therefore starts with every chunk it read reported dirty,
  which is what a host that has drawn nothing needs.
- **The brick cache.** Untouched; this borrows its vocabulary, not its code.

## Impact

`voxel-engine` gains the dirty-set and regional-mesh requirements.
`c-abi` gains the two calls. Purely additive: no signature changes, no struct
grows, no enumerator moves. Docs: `docs/05-claycore-library.md`.

Payoff measured on the 0.02 blob, same machine, back to back (Linux desktop,
not comparable to the device baseline): one dab dirties ~2.2 of the 81
occupied chunks and re-meshes in **0.65-0.68 ms against 23.3 ms** for the
whole grid — **35x**, and inside the 4.17 ms share with room for the host to
draw. The brick cache's equivalent is 0.64 ms against 22.6 ms for all 232
bricks. Dirty tracking is charged to the write path, which is what it costs:
rasterizing a million cells goes 302 -> 305 ms (+0.8%) and a size-8 sphere
stamp 0.0074 -> 0.0086 ms (+0.0012 ms, 0.03% of the frame share).
