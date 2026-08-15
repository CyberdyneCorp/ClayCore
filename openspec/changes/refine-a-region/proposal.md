# Refine a region, not the whole lattice

## Why

`add-multi-resolution` landed the level stack and listed this as the cost it
accepted:

> **Only whole-grid subdivision.** A level covers the whole lattice; there is no
> way to refine one region and leave the rest coarse. Memory therefore follows
> the bounding content rather than the surface.

Measured on the three-primitive form `docs/09` uses, at cell 0.04:

| level | cell | occupied cells | |
|---|---|---|---|
| 0 | 0.040 | 7,328 | |
| 1 | 0.020 | 58,624 | ×8.0 |
| 2 | 0.010 | 468,992 | ×8.0 |
| 3 | 0.005 | 3,751,936 | ×8.0 |

Exactly 8× per level, over the **occupied volume** — and occupancy is
volumetric, so the same solid is 7,328 cells against 2,640 surface triangles.
The proposal's own motivating example is only half answered: a helmet's skull no
longer has to be *authored* at 2 mm, but it is still *stored* at 2 mm the moment
the level exists.

## What Changes

A level gains a set of **refined chunks**. Where a chunk is not refined, the
level's value at a cell is its parent's value — not "empty".

That last sentence is the whole design. The lattice stays uniform and complete:
every cell of every level still has a value, integer cell coordinates are
untouched, O(1) neighbour indexing is untouched, and meshing a level is
unchanged because reads still answer everywhere. **Only what is stored changes.**

It follows that this does not reopen the discrete-levels decision the original
change made, and does not go near `cell_threshold`'s integer-coordinate hash —
the one thing making a soft stroke reproducible across platforms.

Refinement is at **chunk** granularity (32³ cells), because a chunk is already
the storage and indexing unit. A region is rounded out to the chunks it touches
rather than being tracked as an arbitrary box, which is what keeps a stray write
near a boundary from re-materialising everything in between.

### What happens at the edges

- **A write to an unrefined chunk refines it**, seeded from the parent so the
  solid does not move. A brush that straddles the boundary works, and memory
  follows what was actually touched rather than what was reserved.
- **Refinement is upward-closed**: refining a chunk implies its ancestors are
  materialised, because a fine detail has to be representable coarsely for
  `propagate_down` to have somewhere to write.
- **`add_level()` with no region stays exactly what it is today** — the whole
  lattice, every chunk refined. Existing documents and existing callers see no
  change at all, and the serialised form of such a grid is byte-identical.

## Impact

- `voxel-engine` — level storage, the read fallback, the write materialisation
- `bindings` — `clay_voxel_add_level_region`, `pyclay` `add_level(region=...)`
- The `.clayspace` voxel tail gains the refined-chunk set per level, tagged so an
  older reader still opens the grid

## Out of scope

- **Re-measuring the coarse-level meshing workaround on device** (#89's point 2).
  That needs the reference iPad and `tools/run_device_bench.sh`; it is not
  something this change can close.
- **Interpolating up-sampling.** Subdivision still splits a cell into eight
  children with the same index — exact, and blockier than authoring at the fine
  level. Separate concern, unchanged here.
- **Level-aware `rasterize_tape`.** Listed after this one in the issue's own
  order, and it wants this to exist first.
