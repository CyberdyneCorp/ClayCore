# Proposal: a stamp's strength reaches relief and incise

## Why

Found while building a ClayBuildup example. `stamps_to_nodes` places each stamp
at its position, rotation and radius and **drops its strength**, so on an SDF
layer the stroke engine's pressure-strength channel and its `Accumulation` do
nothing. Buildup and clamped produce identical geometry, measured: two dense
strokes of nine overlapping stamps both lifted the surface by +0.196.

That is the control ZBrush's ClayBuildup is named for. Without it the brush has
no buildup.

## Why it was dropped, and why relief is different

Dropping it was right for the ops that existed then. A boolean has no partial
application: a union at half strength is not a smaller union, it is a union of a
different shape, and the radius channel already expresses that. There was
nothing for a strength to scale.

Relief and incise changed that. Their `blend.k` is an **amplitude** — how far the
accumulated surface moves along its own normal — and half an amplitude is
exactly half the displacement. So strength has a meaning here that it does not
have for add, subtract or intersect, and scaling the amplitude by it is the
whole change.

## Scope

Only where `blend.k` is an amplitude, which today is relief and incise. The
other ops read `blend.k` as a radius, a depth or a half-thickness, and scaling
those by a stroke's strength would change the shape rather than the amount —
silently, and differently per op. They keep ignoring strength, and the brush
reference says so rather than leaving a reader to infer it.
