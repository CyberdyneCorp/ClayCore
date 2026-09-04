## Why

Issue #452: every `clay_layer_move_surface` grab makes every later evaluation of
that layer permanently dearer, linearly, and only a full consolidation gives it
back. Reproduced on `main` at `2ae87004`:

| grabs | 0 | 1 | 4 | 8 | 12 |
|---|---|---|---|---|---|
| 16,000 probes | 0.656 ms | 0.907 | 1.309 | 1.818 | **2.364** |

Dead linear at ~0.14 ms a grab (the reporter measured 0.24 with a wider drag),
and it never comes back down.

**It is not a defect, and the issue says so itself**: "a warp is a domain
deformation and two of them do not obviously commute into one". A grab is
recorded as a warp on every item it reaches and each is evaluated per sample for
the life of the edit list. Composing them is a research question, not a fix, and
this change does not pretend otherwise.

What it does is the three things that ARE soundly available — the issue's own
asks, in its own priority order minus the one that cannot be done honestly.

## What Changes

**1. A culled tape drops a warp it cannot reach.** Measured: 512 probes taken
well away from twelve grabs cost 3.20x what the same probes cost with none, and
each of those grabs had touched only 8 of the document's 97 items. The work was
being done for samples that provably could not be affected, because
`cregion_weight` is zero outside the radius.

Sound by induction along the chain: deformers warp in authoring order, so if
every warp so far has been the identity over the region the point is where it
started and the next may be tested against the same region. A warp that is KEPT
may move the point, so the region is dilated by the most that warp can move it
before the next test.

| a brick-sized region | tape params | 512-probe eval |
|---|---:|---:|
| far from every grab | 32 (no warps) | **0.0052 ms** |
| under the grabs | 176 (all twelve) | 0.0824 ms |

**This helps the per-brick paths only** — a refill, and meshing with gradient
normals. A whole-document evaluation has no region to test against and pays for
every warp, so the table at the top of this proposal is unchanged by it. Said
plainly rather than left for someone to discover.

**2. `clay_layer_warp_cost` — what the accumulation is charging.**
`clay_layer_consolidation_cost` answers what BAKING would cost; a host's first
question is whether the layer has accumulated enough to be worth baking, and
there was no way to ask. It reports the items, the warped items, the warps, and
how many of those a culled tape can drop — the gap between the last two being
what working in bricks wins.

**3. The header says a grab is not free after it lands.** The issue's fourth ask,
and its own words for why: "`clay_layer_move_surface`'s documentation reads like
a bounded local edit, and its lasting cost is what surprised us."

## What is NOT changed, and why

**Composing successive grabs.** Two warps do not compose into one in general,
and the sound special cases are narrow enough that the common stroke would not
hit them. Shipping a composition that is wrong in the general case is silently
wrong geometry.

**A cheaper flatten than consolidation.** Resolving a warp into the edit list
means baking the displacement into each item, which is exact only where the warp
is rigid over that item's support — it is not.

Both stay open, with the measurement above as the case for whoever takes them.

## Capabilities

### Modified Capabilities
- `scene-model`: a culled compile drops a finite-support warp its region cannot
  reach, as it already drops an item.
- `c-abi`: a layer reports what its accumulated warps cost.

## Impact

- `src/scene/tape_build.cpp` — the deformer cull.
- `bindings/c/clay.h`, `clay_c.cpp`, `bindings/python/` — the cost query.
- `tests/unit/`, `bindings/python/tests/`.
- **ABI 0.79.0 -> 0.80.0.**
