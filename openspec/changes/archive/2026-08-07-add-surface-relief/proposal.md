# Proposal: surface relief

## Why

Issue #7 calls this "the defining gap", and its own priority list puts it first:
ZBrush Standard, ClayBuildup, Crease and DamStandard, Blender Draw and Clay
Strips, and every 3DCoat surface-mode brush **displace the existing surface
along its normal**. Every brushing primitive here adds or removes *items*
instead. Without relief, sculpting in an SDF layer is accretion and booleans.

## The design question, and it answers better than expected

The question worth settling before writing anything was whether an item can act
on the field of everything *before* it, since that is what "displace the
existing surface" means and no primitive can see past itself.

It can, and the mechanism is already there. Every op goes through
`ctape_combine_values(a, b, mode, profile, k, r2)`, where `a` is the accumulated
field and `b` is the item's own — both, already, for every op.

And there is an exact precedent for using the second as a *region* rather than
as a shape: `ccombine_paint` reads `b.d`, derives a weight from it, and modifies
only the accumulator's colour, leaving the field untouched. Relief is that with
the field instead of the colour.

So **relief is a combine op**, not a new primitive, not a new tape mechanism,
and not a change to how items are stored or evaluated. That is a much smaller
row than the issue's framing suggested, and it is smaller because the
architecture already had the shape.

## What it does

    r.d = a.d - amplitude * falloff(b.d)

Offsetting a distance field moves its isosurface along the field's own gradient,
which is the surface normal — so "offset the distance" and "displace along the
normal" are the same operation, not an approximation of it. That is why the
issue's suggested formula is exactly right.

The weight comes from the item's own field, the way paint's does: full inside,
tapering to nothing across a stated width, and exactly zero beyond it. The item
is therefore a *region*, and any primitive can be one — a sphere for a round
brush, a box for a chisel, an extruded polygon for a stamp outline.

**Two ops, not one signed amplitude.** This was scoped the other way, arguing
from magnify and pinch, and that was the wrong precedent: a deformer carries its
parameters in a free-form float array where a sign costs nothing, while an op
carries `blend_k`, which is validated non-negative in three places — including
the blend constructor, which has no op to be aware of. The sign has nowhere to
live.

It is also the convention already here. Add and subtract are a pair; so are
engrave and emboss. Relief (build up: Standard, ClayBuildup) and Incise (cut in:
Crease, DamStandard) follow them, and share one kernel branch with the sign
chosen from the mode, so they cannot drift apart — the same way `sculpt_pinch`
and `sculpt_magnify` share one walk.

## Where the parameters live

The existing convention rather than a new one. `clay.h` already documents that
"for the extended modes blend_k is the mode's radius or depth, and groove and
tongue additionally read the item's rounding as the channel half-width". Relief
follows it: **blend_k is the amplitude** and **the item's rounding is the
falloff width**.

## What this buys, and what it does not

It covers the relief half of issue #7 item 1. It does NOT by itself make strokes
glide on a surface — that is item 4, which needs the stroke resolver to project
onto the document, and the projection primitive for that already exists
(`pick::snap_to_surface`). The two together are what the issue says makes it
"feel like ZBrush"; this is the first of them and they are independent.

## Exactness

Offsetting the accumulator by `amplitude * falloff` raises the field's slope by
that term's gradient, which is `|amplitude| / width` — the falloff is the only
thing varying. Same structure as every region-limited operation added recently,
and it means a deep relief through a narrow falloff costs the marcher, visibly
and by a declared amount.

Support is finite, so item influence bounds and brick culling are unaffected —
which is the property that makes this usable at stroke densities at all.

## What this change does not do

- **No alphas.** A grayscale stamp driving the amplitude is issue #7 item 3,
  and needs an image lifted into the field; the region here is a primitive.
- **No surface-conforming resolution.** Issue #7 item 4, independent.
- **No relief on voxels.** They have `sculpt_inflate`, which is the same idea in
  the representation that already had it.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `c-abi`, `python-bindings`.

## Impact

- `include/clay/kernel/tape.h` (one combine mode), `exactness.h`,
  `src/scene/bounds.cpp`, `scene/types.h` for the op, the C ABI, the Python
  bindings, the parity corpus, tests, docs, an example.
