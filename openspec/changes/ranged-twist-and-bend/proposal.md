# Proposal: twist and bend, confined to a span

## Why

Stage 1 of #116. ZBrush's Gizmo 3D deformers act **inside the gizmo's box**;
`twist` and `bend` here act on the whole item. That is the entire gap for two of
the four gizmo deformers the issue cares about, and it is the smallest one — it
fits the existing deformer slot budget and needs no new payload machinery.

The practical difference: winding the whole item means a column's far end keeps
turning. An artist twisting the middle of a form wants the ends to travel
rigidly, which is what a bounded gizmo does and what no existing deformer here
can express.

## What

`twist_range(k, y0, y1, ease)` and `bend_range(k, x0, x1, ease)` — the SAME
rotations with the angle ramped across a span and held beyond it.

Deliberately the same rotation with a substituted angle rather than a second
formulation: with a linear ease and a span covering the content,
`twist_range(k, 0, 1, linear)` **is** `twist(k)` at every point inside the span,
which is asserted rather than assumed. That is what makes this a range on an
existing deformation rather than a new one to keep in step with the old.

## The Lipschitz is charged the rate the ease REACHES

An eased ramp is steeper somewhere in the middle than its average rate —
smoothstep peaks at 1.5× linear — so the bound uses `k · ease_max_slope`, the
same `ease_slope` convention `grab`, `pose` and `magnify` already follow. A bound
derived from the average rate would be under the field's actual slope where the
ramp is steepest, and the raymarcher would step through the surface there.

The AABB hull is shared with the unranged forms unchanged: a bounded rotation
about an axis is contained by the cylinder the unbounded one sweeps.

## Not this

- **Not Bend Curve** (#116 stage 2): a bend along an arbitrary guide, which
  needs the curve payload and inverse parallel transport.
- **Not the Lattice / FFD** (#116 stage 3): the interesting one, because a
  claycore deformer is an inverse point map and forward FFD has no closed-form
  inverse. The issue records the three candidate answers; none is taken here.
- **No new payload.** Both fit the existing slot budget, which is why this stage
  is separable at all.

## Impact

- Two enumerators appended to a serialization-stable enum — no existing value
  moves, no record grows.
- `sdf-kernels` gains the requirement; `build-packaging`'s statement of what the
  parity fixture covers goes from 5 of 14 deformers to 7 of 16, measured.
- Reachable from both bindings and gated by `check_binding_parity.py`; a parity
  corpus scene with an EASED ramp, so a backend that ignored the ease fails.
