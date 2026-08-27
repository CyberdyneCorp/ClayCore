# Proposal: drag a squashed item

## Why

A gap in #320, found while scoping the layer arm it deferred.

`brush::move` composes an item's world frame as `layer.xform * node.xform` and
stops there. That was the whole story until #320, and its comment said so:
*"the evaluator composes exactly these two, and a group in between contributes
nothing."* It is no longer true. An item now also carries a per-axis scale,
applied INNERMOST, and the tape takes it off as part of the whole inverse
BEFORE the deformer chain runs — so the space a `grab` deformer lives in is not
the placed frame the brush authored it in.

The result is not a subtle inaccuracy. Measured on a unit sphere scaled 3x on
X, whose surface therefore sits at world x = 3:

    uniform    grab at (1, 0, 0)   field 0.0 -> 0.077   the surface moved
    squashed   grab at (3, 0, 0)   field 0.0 -> 0.0     NOTHING HAPPENED

The placed frame maps world (3, 0, 0) to local (3, 0, 0), which is two units
outside a unit sphere, so the grab's falloff reached no part of the item at all.
An artist dragging the surface of a stretched object sees the tool do nothing.

## What this changes

The frame composition, in one place, plus the two mappings that come off it:

- the grab CENTRE, a point, takes the per-axis scale off last because it is
  innermost;
- the DISPLACEMENT, a vector, takes the same;
- the RADIUS, which is where it stops being mechanical.

**The radius is a choice and the proposal states it rather than burying it.** A
`grab` carries one scalar radius, and a squashed frame turns the artist's
world-space sphere into a local ellipsoid, so no scalar is exact. Dividing by
the LARGEST factor is the conservative reading: every world reach
`R * s_i * scale` is then at most the radius circled, so a drag never takes
geometry the artist did not enclose. Under-reach is recoverable by dragging
again; over-reach is not. It is the same instinct `cscale_nu_dist` follows when
it multiplies by the smallest factor so a distance is never overestimated.

## What it does NOT change, and why

`brush::lattice_gizmo` has the identical bug — its comment even names the
assumption that broke: *"Rigid with uniform scale on both sides, so the
composition is too."* It is not fixed here because it cannot be without a
FORMAT change: the gizmo hands the deformer a `local_to_cage` placement, and
`Deformer::cage_xform` is a `math::Transform`. The map it now needs is
`Transform ∘ diag`, which is not a Transform and has no closed form as one, so
the deformer has to carry more than it can carry today.

That belongs with the layer per-axis scale, which forces the same widening for
its own reasons and takes a format minor anyway. Named here so it is a recorded
gap rather than something the next reader rediscovers from a wrong-looking cage.

## Impact

`scene-model` gains the requirement that a warp is authored in the space the
deformer chain runs in. No ABI symbol is added or removed and no signature
changes: one existing verb produces a correct result where it produced an inert
one. Patch release 0.54.1, no format change.
