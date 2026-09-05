## Why

Issue #452 reports that a `move_surface` grab "permanently costs every later
evaluation of the layer (+0.24 ms per grab, linear)". Re-baselined on `main`
across every host-facing path, on an idle box, the shape is different and much
worse than that:

| grabs | warps | eval_points | gradients | raycast | mesh | brick refill | safe step |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0.29 ms | 1.24 | 10.2 ms | 35.3 ms | 0.0019 ms | 1.00000 |
| 12 | 248 | 1.34 | 6.6 | 81.5 ms | 172 ms | 0.0013 ms | 0.10322 |
| 50 | 1,673 | 6.48 | 32.0 | **1000 ms** | **1792 ms** | **0.0008 ms** | **0.00008** |

Two things that were not known before:

**The brick refill is already flat.** `price-the-warps-a-layer-carries` (#457)
fixed the path a stroke actually redraws through. That half of #452 is closed.

**What is left is not a per-warp cost — it is the STEP SCALE.** It collapses to
8e-05, so the marcher takes ~12,500x more steps, and raycast and meshing are
79x and 44x worse. Those are the two things a viewport does continuously.

## What Changes

The declared Lipschitz of a deformer chain, in two places, both sound and both
independent of the number of brushes:

- **The travel budget between two links is the travel BETWEEN them**, not the
  whole chain's. A point starting in link i's ball is carried toward link k's by
  the links in between; summing every link's travel means each new grab widens
  the budget every other pair is measured against. Fifty drags of 0.05 gave a
  budget of 2.5 against balls of radius 0.3, so every pair on a form 1.7 across
  counted as able to meet.

- **Each link is priced against its own NEIGHBOURHOOD, not its connected
  component.** The old grouping was a union-find over "these two can meet", and
  that relation is TRANSITIVE where "both act at one point" is not: balls A-B
  and B-C may overlap with A and C disjoint, and no point sees all three. A
  stroke chained every drag into one group and charged the product of the lot.

  Still an upper bound, and the argument is short: every link acting at one
  point contains that point, so they pairwise can meet, so they all lie inside
  any one of their neighbourhoods.

## Result

Sixteen grabs walked along a bar, each overlapping only its neighbours:

| | safe step scale |
|---|---:|
| 2 grabs | 0.748 |
| 16 grabs, before | **0.098** |
| 16 grabs, after | **0.647** |

**6.6x**, and eight times the drags now costs almost nothing rather than eight
compoundings. On the issue's own fixture at 12 grabs: step 0.103 -> 0.469, and
a raycast 81.5 -> 51.6 ms.

`check_conservative_steps` runs on the relaxed chain, because a step scale that
is too large is a marcher that walks through the surface, and the gate fails
against the old code -- 0.098 is outside it.

## What this does NOT close

At 50 accumulated grabs raycast and meshing are still ~1000 ms and ~1800 ms. The
step scale is no longer the whole story there: 1,673 warps are evaluated per
sample, and neither route out works today.

- **Per-region culling cannot help.** It already fires -- a pick compiles a
  ray-local tape whenever the step scale is under 1 -- but a ray crossing a form
  touched by fifty drags legitimately passes through many of their balls.
- **Regional consolidation reclaims everything and costs too much.**
  `clay_layer_consolidate_region` takes the warps to 0, the step back to 0.577
  and meshing from 1841 ms to 13.6 ms -- but 19.4 s over the worked region, and
  122 s if each gesture's own patch is absorbed as the header suggests, because
  each closure swallows the volumes before it.

Bounding the warp COUNT is the remaining half, and it is the design question
`price-the-warps-a-layer-carries` already parked: composing successive grabs is
not algebraically available, and a flatten cheaper than consolidation does not
exist yet. This change does not attempt it.

## Capabilities

### Modified Capabilities
- `scene-model`: what a chain of finite-support brushes is charged for.

## Impact

- `src/scene/bounds.cpp` -- the grouping and the travel budget.
- No ABI change, no format change. The bound only ever gets smaller, so nothing
  that was safe to march becomes unsafe.
