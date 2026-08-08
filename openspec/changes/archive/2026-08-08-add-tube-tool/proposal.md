# Proposal: the Tube tool

## Why

Nomad Sculpt's Tubes: tap or draw a path, get a rope, pipe, tentacle or hair
strand along it, with a radius that can vary along its length, a cross-section
that need not be a circle, and a B-spline toggle between smooth curves and sharp
corners. It is the tool a sculptor reaches for constantly, and it is the one
place where every ingredient here is already built and none of them are joined.

- **Control points and the B-spline toggle** — `StrokePointType` is already
  Hard, Spline, BSpline and Bezier, tessellated to a document tolerance.
- **A round tube with a varying radius** — the stroke opcode already sweeps a
  sphere along a segment chain with a radius PER POINT, and it stays EXACT.
- **A cross-section that is not a circle** — `Prim::swept` already carries
  profiles along a guide on parallel-transported frames.
- **Closed tubes** — `stroke_closed` already exists.
- **Snap to a surface** — `pick::snap_to_surface` already exists.

What does not exist is the step that turns a drawn path plus a handful of
settings into the right item. Leaving that to each caller means every one of them
answers "how does the radius vary along the curve", "when is this a stroke and
when a sweep" and "what does closed mean for a tapered tube" differently — the
same argument the cut tool and snakehook were built on.

## What it is

A **resolver**: points and settings in, an ordinary edit item out. Pure, so a
caller can preview a tube before committing it.

It chooses the representation rather than exposing the choice as a second way to
say one thing:

- No profile: a **stroke chain**, which is a swept sphere and therefore EXACT —
  the safe step scale stays 1.0. This is Nomad's default round tube.
- A profile: a **swept item**, which is a bound field and costs step scale. That
  is the price of a square or custom cross-section, and the resolver says so
  rather than letting it surprise a caller.

The radius varies through a start/middle/end triple, as Nomad's handles do,
distributed by ARC LENGTH so a path whose points bunch does not bunch the taper.

## What it is not

Not a new primitive, not a new opcode, and not a new curve type. Everything it
produces already round-trips, undoes, picks and meshes, because it produces the
items those already handle.

Not "Validate". Converting to a polygon mesh is what the meshers already do to
any document, and a tube is not special in needing it.

Not Snap. `pick::snap_to_surface` already places a point on a surface, and the
resolver takes the points it is given — no camera and no picking enters here,
the same rule the cut tool follows.
