# Price a deformer chain where its links can reach

## Why

`deformer_lipschitz` folds a chain into one running product, so every pair of
links is charged as though it compounds. Most pairs cannot.

`GRAB`, `MAGNIFY`, `POSE`, `BLOB` and `ALPHA` are all documented as having
FINITE SUPPORT — `clay.h` puts it as "outside the radius the field is untouched,
which is what makes it a brush rather than a modifier". Two whose balls are far
apart have no point at which both are anything but the identity, so their
factors have nothing to multiply.

Measured (issue #386): eight `move_surface` drags of radius 0.3 around the
equator of a radius-4 sphere, centres 3.06 apart — ten radii from touching:

| drags | safe step scale, before | after |
|---:|---:|---:|
| 1 | 0.7059 | 0.7059 |
| 2 | 0.4983 | 0.7059 |
| 4 | 0.2483 | 0.7059 |
| 8 | 0.0616 | 0.7059 |

That is the whole cost of a Move-heavy session, charged for a compounding that
cannot physically happen. A sculptor nudging eight different places on a head is
the NORMAL case — the drags are disjoint almost by definition, since a second
drag on the same spot is a correction rather than a stroke. The reporting host
measures a twelve-drag session taking a gesture from 7.3 ms to 59.5 ms, with the
step scale underflowing to zero by the ninth drag.

## What Changes

- Each link of a chain is priced ON ITS OWN rather than folded into a running
  total, and carries the region outside which it is the identity.
- Links whose supports cannot reach one another are grouped, and the chain's
  bound is the WORST group rather than the product of all of them. A link with
  unbounded support acts everywhere and is charged against every group.
- The composition keeps the distinction the fold always had: a point warp
  MULTIPLIES the slope through the chain rule, a distance offset ADDS its own
  gradient to it.

## The argument

The chain evaluates `p -> d0 -> d1 -> ... -> prim`, so link k sees the point
after links 0..k-1 have moved it, and link k is the identity unless that point
lies in its ball. Two links can therefore both act on one evaluation only if
their balls are within the distance the chain can carry a point between them.
Bounding that by the TOTAL travel of every point warp in the chain is
conservative in the safe direction and needs no ordering argument.

One residual, stated rather than glossed: a single entry of the easing table
returns 5.96e-08 rather than 0 at its zero end. It costs nothing, and not by
luck — the region weight is CLAMPED, so outside the ball that value is a
CONSTANT. A constant weight makes a grab a rigid translation and a magnify a
uniform scale within 1e-7 of the identity: no slope, which is the only thing a
Lipschitz bound measures, and the translation is inside the travel budget above.

## Impact

- Affected specs: `sdf-kernels`
- Affected code: `src/scene/bounds.cpp`
- No API, ABI or format change. The number `clay_layer_safe_step_scale` reports
  gets larger for a chain of disjoint brushes and is unchanged for every other
  document, so a host marches by it faster and nothing else moves.
