# Proposal: voxel repair

## Why

A voxel layer that has been sculpted into a perforated or hollow state cannot
be made airtight before meshing, and "Close Invisible Holes + Fill Voids" is
the standard pre-bake step in the tool this engine is measured against.

This row is lower priority than it sounds, and it is worth saying why so nobody
over-invests: SDF layers are watertight by construction, and a voxel grid's
greedy mesh is closed by construction too — every face between an occupied and
an empty cell is emitted. What "not airtight" means here is narrower and real:
a shell with single-cell perforations lets the outside reach the inside, and
enclosed pockets of empty cells survive into the bake as interior surface
nobody wants.

## What this shares with the verbs

`add-voxel-verbs` and this change share **morphological closing**, not the
connected-component pass the Phase 2 plan guessed at. Filling a concavity and
closing a perforation are the same dilate-then-erode at different scopes: the
verb does it inside a footprint, this does it over the grid. The closing itself
is written once.

Only **fill-voids** needs a flood, and it needs one the engine does not have:
`flood_select` walks *occupied* cells from a seed, while this walks *empty*
cells inward from outside the bounds. Everything the flood cannot reach is
enclosed, which is the definition doing the work.

## Report before repair

Repair is destructive and its input is somebody's sculpt, so it reports first:
how many enclosed voids there are and how big the largest is. A caller can then
decide, show it, or skip. Their own bug list asks that every destructive
operation be preview-committed; the cheap version of that is being able to ask
the question without performing the answer.

## What Changes

- **`repair_report`** — enclosed void count, their total volume, the largest,
  and whether the grid is already airtight. Non-destructive.
- **`repair_close_holes(radius)`** — morphological closing over the grid,
  sealing perforations up to that radius.
- **`repair_fill_voids()`** — floods empty cells from outside the padded
  bounds; every empty cell it cannot reach is filled.
- Both repairs honour a mask, so a frozen region is not repaired either. A
  repair that ignored freeze would be the one destructive operation in the
  engine that does.

## Capabilities

### Modified Capabilities

- `voxel-engine`, `python-bindings`, `c-abi`.

## Impact

- `include/clay/voxel/grid.h`, new `src/voxel/repair.cpp` sharing the verbs'
  morphology helper, both bindings, tests, docs, an example.
- ABI 0.17.0 — additive, landing alongside `add-voxel-verbs`.
