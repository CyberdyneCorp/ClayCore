# Proposal: Trim Curve

## Why

Asked for ZBrush's Trim Curve. The cut tool already resolves a drawn shape into
an ordinary extruded item, and `CutShape::from_curve` already flattens a
control-point curve through the same tessellator a curve item uses.

It is the wrong shape for a trim, though, and by one word: it tessellates with
`closed=true`. That makes it a spline **lasso** — a closed outline whose inside
is the cut. A Trim Curve is an **open** stroke drawn across the form, and what
it removes is everything to one side of it. Neither is expressible as the other:
closing an open trim stroke joins its endpoints and cuts a sliver instead of a
half.

## What it is

`CutShape::from_open_curve` — the same tessellator with `closed=false`, then the
polyline closed against the frame's own bounds on the side being discarded. The
result is an ordinary polygon outline, so it reaches `cut_item` unchanged and
becomes an ordinary extruded item like every other cut.

Which side goes stays where the cut tool already puts it: the OP. Place the
result with `Subtract` to remove the covered half, `Intersect` to keep it. A
`side` naming the half the polygon covers is not a second way to say that — it
is which half the polygon *is*, and the op still decides what happens to it.

## What it is not

Not a new cutting mechanism, and not a new primitive: it is a polygon outline
built a different way. The prism rule holds — a trim is a straight cut, so the
sweep is parallel and the caller passes the frame, because no camera enters the
engine.

Not a curve that closes itself around the form. The stroke is assumed to span
the region it divides, which is how a trim is drawn; a stroke that stops short
leaves the closing edge visible in the result, and that is the caller's to see.
