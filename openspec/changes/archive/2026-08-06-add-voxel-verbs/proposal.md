# Proposal: the remaining voxel verbs

## Why

Four sculpting verbs exist — smooth, inflate, flatten, pinch. Against the set
the study catalogues, four are missing: fill-cavities, scrape, smudge and
carve-with-alpha. They are the difference between "the engine can push
material around" and "an artist can finish a surface".

## Two corrections, both found by writing it

The Phase 2 plan guessed that this row and `add-voxel-repair` share "a
connected-component pass". They do share something, but not that.

**They share a pocket-fill rule, and it is not morphological closing.** Closing
was the obvious answer and the first attempt, and it is wrong here for two
reasons the code found rather than the design predicted. A ball of radius r
*fits into* a dent wider than r, so a larger structuring element fills **less**,
not more — a radius-2 closing declines to fill a 2×2 dent that a radius-1
closing seals. And a closing cannot seal a one-cell perforation in a one-cell
wall at all: the erosion reaches through from the void behind and reopens every
hole the dilation just closed. Both are precisely the cases this exists for.

What works is local and blunt: **an empty cell with at least four of its six
face neighbours occupied is inside a pocket.** A flat face gives one, a concave
edge two, a corner three — so four is the line between "irregular surface" and
"hole", and smoothing is already the verb for the former. This change applies
that rule inside a footprint; repair applies the same rule over a whole grid.

**Only fill-voids needs a flood**, and it needs one the engine does not have:
`flood_select` walks *occupied* cells from a seed, while enclosure needs a walk
over *empty* cells inward from outside the bounds. That lives in the repair
change.

## The scoping decision, made up front

`carve-with-alpha` needs an alpha, and there is no texture pipeline. The plan
said to settle this before starting rather than during, so: **the caller
supplies the alpha as a scalar grid**, with its own width and height, projected
along a direction the caller gives. A host that has an alpha has already loaded
a PNG; handing us the samples costs it nothing and costs the engine no image
decoding, no colour management and no format zoo. When a texture pipeline
arrives it feeds this entry point rather than replacing it.

## What Changes

- **`sculpt_fill_cavities`** — closes concavities inside the footprint up to a
  stated width. A through-hole and an open surface are left alone, which is
  what makes it "fill the dents" rather than "fill everything".
- **`sculpt_scrape`** — flatten and smooth as **one** pass over **one**
  snapshot. Calling the two existing verbs in sequence would let the flatten's
  output feed the smooth's neighbourhood, which is precisely what the
  snapshot discipline exists to prevent; scrape decides both from the state it
  found.
- **`sculpt_smudge`** — drags *surface* material along a direction, leaving the
  interior where it was. That is what distinguishes it from grab, which
  translates every cell in the region through an inverse map: grab moves a
  lump, smudge smears a skin.
- **`sculpt_carve_alpha`** — the footprint's per-cell strength is modulated by
  the caller's alpha grid, sampled by projecting each cell onto the plane
  perpendicular to the carve direction.
- All four honour the mask and the falloff exactly as the existing four do,
  because gating lives in the shared footprint walk rather than in each verb.

## Capabilities

### Modified Capabilities

- `voxel-engine`, `python-bindings`, `c-abi`.

## Impact

- `include/clay/voxel/grid.h`, `src/voxel/sculpt.cpp`, a shared morphology
  helper, both bindings, tests, docs, an example.
- ABI 0.17.0 — additive.
