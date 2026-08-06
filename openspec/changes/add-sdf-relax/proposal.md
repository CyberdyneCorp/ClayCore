# Proposal: relaxing an SDF surface

## Why

The last ZBrush core brush. Voxel layers have `sculpt_smooth`; SDF layers have
nothing, and the only route today is the one-way voxel bridge.

## Settling the design question

The roadmap named three routes and said the row's size varies by an order of
magnitude across them, so settle it before writing anything:

- **(a) Round-trip through a sampled field.** Needed `add-sampled-fields`.
  Gives a bake, not a live edit.
- **(b) A field-space local re-blend.** Needs no prerequisite, but changes what
  an edit list *means*.
- **(c) Mesh-space extract, smooth, re-import.** Same prerequisite as (a) plus
  the cost of a meshing round trip.

**(a), and the reason is now stronger than "it is cheapest".**

(b) is the one that sounds best and is worst. Re-blending in field space means
every item's contribution is reweighted by its neighbours, so the edit list
stops being a list of shapes and becomes a list of shapes *plus a rule about
how they interact*. Undo, picking, serialization and the C ABI all describe an
edit list; that change would reach all of them, and the result would still not
be a general relax — it can only smooth where two items meet, not where one
item is simply bumpy.

(c) is (a) with a meshing round trip in the middle, which loses the interior
and costs more. There is nothing it buys.

So relax **samples the field, smooths the samples, and hands back a volume** —
which since `add-sampled-fields` is an ordinary item that saves, evaluates on
four backends and combines with everything else.

## Smoothing a distance field is not the disaster the roadmap feared

The roadmap says convolving a distance field "breaks the distance property the
evaluator depends on". That is half right, and the half that is wrong is the
half that decides whether this is safe.

Convolution does destroy **exactness** — the smoothed field no longer reports
the true distance to its own surface. But it *preserves the Lipschitz bound*,
and averaging can only shrink it: for weights summing to one,

    |f̄(x) − f̄(y)| ≤ Σ wᵢ |f(x+dᵢ) − f(y+dᵢ)| ≤ |x − y|

And a 1-Lipschitz field is **automatically a conservative bound on the distance
to its own zero set**: if z is the nearest zero to x, then
|f(x)| = |f(x) − f(z)| ≤ |x − z| = dist(x, Z). So sphere tracing on a smoothed
field cannot overstep. The property the evaluator actually depends on survives;
it is only the claim of exactness that does not, and a sampled volume already
declares itself inexact.

That is what makes this row small rather than large.

## What Changes

- **`field::relax`**: smooth a sampled volume, returning a new one. The
  smoothed value at a point is a weighted average of the field around it, and
  the result is re-sampled through the existing narrow-band builder — so brick
  boundaries, the halo, the sparse index and the far-field bounds are all
  handled by machinery that already works rather than by a second copy of it.
- **A region**: a centre, a radius and a falloff, so relax is a *brush* and not
  only a global filter. Outside the region nothing moves.
- **Iterations**, because one pass of a small kernel is a small effect and a
  large kernel is not the same thing as several small ones.
- **Python bindings**, a C ABI entry point so an imported scan can be smoothed
  from an app, and an example.

## The honest cost, stated where a caller sees it

Relax **bakes**. The document that comes back is a sampled volume, not the edit
list that went in: the items are gone, and with them the ability to go back and
change a radius. That is inherent to route (a) and is not a limitation of this
implementation — a general relax has to be able to smooth a bump in the middle
of one item, which no reweighting of an edit list can express.

The bindings say so, the example demonstrates it, and the resolution is the
caller's to choose because it is the resolution the shape now has.

## What this change does not do

- **No live/parametric relax.** Named above.
- **No curvature-weighted smoothing.** Uniform weights within the kernel; a
  curvature-weighted variant is additive.
- **No way to relax a DOCUMENT from C.** The C ABI can build a volume from a
  mesh but not from a document, so from C the reachable workflow is "import a
  scan, then smooth it" — which is the one an app wants, and which this row
  does provide. Sampling a document through the C ABI is its own decision and
  does not belong here.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `python-bindings`.

## Impact

- New `include/clay/field/relax.h` and its source; `FieldVolume` gains the
  ability to have its stored samples rewritten in place; the Python bindings,
  the C ABI, tests, docs, an example.
