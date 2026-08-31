# A voxel grab as a gesture

## Why

A grab of N cells is not N grabs of one cell, and the difference is not small.

`sculpt_grab` reads the grid, resamples occupancy through the falloff and writes
back, so the next call reads its own output. On top of that the displacement is
rounded to whole cells AFTER the falloff weights it, so at one cell of
displacement only the very middle of the region rounds to a cell — and inside
solid material, moving only the middle changes no occupancy at all. Split
finely enough, the whole drag evaporates.

Measured (issue #393, reproduced on this branch cell for cell) on a solid ball
16 cells across at cell 0.04, smooth falloff, strength 1, front-gated, the same
total drag of 8 cells in +y. "New" counts occupied cells whose coordinate was
not occupied before; occupancy at rest is 2109:

| footprint | 1 × 8 | 2 × 4 | 4 × 2 | 8 × 1 |
|---|---|---|---|---|
| 24 cells | 59 / 2157 | 61 / 2170 | **0** / 2109 | **0** / 2109 |
| 32 cells | 205 / 2200 | 169 / 2215 | 190 / 2298 | **0** / 2109 |
| 40 cells | 357 / 2205 | 376 / 2326 | 293 / 2371 | 126 / 2235 |

Occupancy is not conserved across the split either, so composed grabs smear and
duplicate rather than translate — a different failure from the one that makes
the counts zero, and the one that bites a host splitting coarsely enough to move
anything.

This closed #17 the wrong way round. That issue was about SUB-CELL
displacements, and its advice — accumulate past the cell size host-side, then
emit — is what a host naturally implements after reading the header. It produces
exactly the stream of one-cell grabs that moves nothing. The reporting host
shipped it, measured it doing nothing on seven of eight segments of an ordinary
drag, and fell back to holding the whole gesture until pointer-up. That works
and costs the live preview.

## What Changes

- **ADDED** `voxel::GrabTransaction`, `clay_voxel_grab_begin` / `_update` /
  `_written_box` / `_commit` / `_cancel` / `_destroy`, and `VoxelGrid.grab`
  returning a context manager.
- `begin` captures the material as it is. Every `update` takes the TOTAL
  displacement from the anchor and resamples from that capture, so a run of
  updates ends where a single one to the same total would, repeating an update
  changes nothing, and a pointer that comes back to where it started puts the
  material back.
- `clay_voxel_sculpt_grab`'s note says outright that it does not compose and
  points at the transaction — which is the issue's third option, worth having on
  its own and free once the mechanism is written down.

## The capture grows with the drag

A drag does not say up front how far it will go, so the ring outside the
footprint is captured lazily as the displacement reaches for it. That is sound
because only the FOOTPRINT is ever written: a cell outside it is still pristine
whenever the capture widens to include it. Asking a host to declare a maximum
reach would be one more number to get wrong, and getting it wrong would read
back the gesture's own output.

## What this does not fix

Occupancy is binary and the resample is nearest-cell, so a total displacement
under half a cell on every axis still moves nothing — there is no sub-cell state
for it to move. What changes is that the drag is measured from the ANCHOR, so a
slow drag accumulates toward that half cell instead of rounding to zero on every
frame.

## Impact

- Affected specs: `voxel-engine`, `c-abi`, `python-bindings`
- Affected code: `include/clay/voxel/grab.h`, `src/voxel/sculpt.cpp`,
  `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `bindings/python/pyclay_module.cpp`
- Additive: `sculpt_grab` is unchanged, and a single `update` is cell-for-cell
  what it produces.
