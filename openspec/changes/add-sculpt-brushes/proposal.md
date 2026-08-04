# Proposal: falloff brushes and the sculpting verbs

## Why

The voxel toolkit can place, erase and recolour material. It cannot *shape*
it. Every operation a sculptor reaches for after the block-out — soften this
edge, push this surface out, flatten this face, draw this ridge in — has to be
done cell by cell. That is the gap between "voxel editor" and "sculpting app",
and ClaySpace is the latter.

I previously argued falloff was inapplicable because voxel occupancy is binary
and there is no partial coverage for a falloff to act on. That is true of a
single cell and false of a footprint: a soft brush on a lattice is expressed as
*fractional coverage over the footprint*, which is exactly what dithering a
per-cell weight gives. The result is a brush whose effect fades at the rim
instead of stopping at a hard boundary. It has to be deterministic to be
usable, so the dither is a hash of the cell coordinate rather than a random
number generator: the same stroke on the same grid always produces the same
cells, and re-running an example regenerates identical output.

## What Changes

- **`BrushParams`**: size, shape, falloff curve, strength, and a dither seed,
  passed as one struct. Overloads of `set_brush`/`erase_brush`/`paint_brush`
  take it, alongside the existing plain-size signatures.
- **Falloff curves**: `Constant` (the current hard edge, and the default),
  `Linear`, `Smooth` (smoothstep), `Gaussian`. Weight is a function of the
  normalized distance from the footprint centre, scaled by `strength`.
- **Deterministic dithering**: a cell is affected when its weight exceeds a
  hash of its coordinate and the seed. Weight 1 always applies, weight 0 never
  does, and everything between is stable across runs and platforms.
- **Four sculpting verbs**, all falloff-weighted and all computed from a
  snapshot of the region so results do not depend on iteration order:
  - `sculpt_smooth` — majority filter over the 26-neighbourhood: thin spurs
    dissolve, notches fill, the surface relaxes.
  - `sculpt_inflate(amount)` — dilate for positive amounts, erode for
    negative, repeated `|amount|` times.
  - `sculpt_flatten(normal, offset)` — erase occupied cells on the positive
    side of a plane, fill empty cells on the negative side that touch
    material, pulling the surface onto the plane.
  - `sculpt_pinch` — move surface cells one step toward the brush centre,
    drawing the surface inward.

## Capabilities

### Modified Capabilities

- `voxel-engine`: brushes gain falloff and strength; the sculpting verbs join
  the editing operations.
- `python-bindings`: the verbs and the falloff arguments reach Python.

## Impact

- New `src/voxel/sculpt.cpp`; `include/clay/voxel/grid.h`, `bindings/python/pyclay_module.cpp`, tests, a new example, docs.
- No serialization, undo, or format impact — these are grid ops like the
  existing brushes.
- Non-goals: sub-voxel/anti-aliased occupancy (that is a different data model —
  a density grid, not a palette-indexed lattice), and a general "grab" verb,
  which needs a drag vector and stroke state rather than a single stamp.
