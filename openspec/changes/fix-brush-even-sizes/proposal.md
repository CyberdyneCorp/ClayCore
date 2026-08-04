# Proposal: a brush of size N covers N cells

## Why

`set_brush(c, 4, i)` covers a 3×3×3 block. So does `set_brush(c, 3, i)`. The
radius is `(N-1)/2` and the loop runs `-r..r`, which spans `2r+1` cells — N
for odd N, N-1 for even N. Every even size silently behaves as the odd size
below it, which is the kind of bug a user reads as "the brush size slider does
nothing half the time".

The previous change documented and tested this rather than changing it, on the
grounds that shifting cells under existing callers is worse than a surprising
size. That reasoning does not survive contact with the brush being used: the
sizes are wrong, they were only ever wrong, and nothing outside the repository
depends on the old footprint.

## What Changes

- **Size N covers exactly N cells per axis, for every N.** The footprint runs
  `-((N-1)/2) ..= N/2` (integer division), which is symmetric for odd N and
  biased half a cell toward +X/+Y/+Z for even N — the standard convention for
  an even-diameter brush on a lattice.
- **The sphere radius convention changes with it.** An inscribed radius of
  `(N-1)/2` is degenerate for even N: at N=2 it is 0.5, no cell centre is
  within it, and the brush would stamp nothing. The radius becomes `N/2`,
  measured from the footprint's true centre (which is half-integer for even
  N). The test stays exact integer arithmetic by working in half-units:
  `(2x - (lo+hi))² + ... <= N²`.
- **Odd sizes change too, and better.** Size 3 sphere goes from 7 cells to 19,
  size 5 from 33 to 81. The old convention gave up half a cell on each side,
  so the cell-count ratio converged to `(π/6)((N-1)/N)³ ≈ 0.39`; the new one
  converges to π/6 ≈ 0.524, which is what a sphere of diameter N should
  occupy.
- **The hero image gains the sphere brush**, so the README shows the tool that
  actually builds rounded voxel forms.

## Capabilities

### Modified Capabilities

- `voxel-engine`: brush size means what it says; sphere radius is `N/2`.

## Impact

- `include/clay/voxel/grid.h`, `src/voxel/grid.cpp`, tests, `examples/00_hero.py`, `examples/07_voxel_sculpting.py`, docs.
- Behavioural: every brush call with an even size now covers more cells, and
  every sphere brush covers more cells. No serialization, undo, or format
  impact — brushes are grid ops.
- The previous change's "even sizes round down" scenario is replaced rather
  than deleted, so the spec records the new rule in its place.
