# Proposal: one-sided flatten (hPolish, Planar)

## Why

Asked for ZBrush's hPolish. It is not a new mechanism — it is the **one-sided**
case of the flatten already here.

`field::flatten` blends the source toward the plane's half-space:
`here + (plane - here) * weight`. That is two-sided by construction, and
measured on a ball carrying both a bump and a dent it fills the dent by +0.380
while cutting the bump by -0.220, everything landing on the plane. ZBrush's
Flatten behaves that way and the docs already say so.

hPolish — and Planar, and the Trim family — only ever **plane down**. That
difference is the whole brush: cutting without filling is what leaves a crisp
facet against untouched surface, and it is why the family is the hard-surface
one. Filling the hollows beside a facet is precisely what a polish must not do.

## What it is

A mode on the existing operation rather than a second entry point, because the
math differs by one clamp:

| Mode | Term | Effect |
|---|---|---|
| `TwoSided` | `(plane - here)` | today's behaviour — ZBrush Flatten |
| `CutOnly` | `cmax(plane - here, 0)` | only removes — hPolish, Planar, Trim |
| `FillOnly` | `cmin(plane - here, 0)` | only deposits — the dual, free once the clamp exists |

A separate `polish()` function would be a second way to say one thing, which
this repository already refuses for keep-inner/keep-outer on the cut tool and
for magnify against pinch.

Naming follows the convention the ops use: `relief`/`incise` rather than
"Standard"/"Crease", `magnify` with a signed strength rather than a pinch verb.
The mode says what it does; the brush reference maps the vendor names onto it.

## What it is not

No change to how the plane is chosen. The caller supplies point and normal, as
it does for the cut tool, because no camera enters the engine — a host derives
the plane from the pick and gradient it already has.

No change to the region requirement. Flatten is local by nature: where the
weight is one the result IS the plane, so a region is still required and a
radius of zero is still refused.

`FillOnly` is included because the clamp that makes `CutOnly` possible makes it
free, and because it is the operation that fills a scanned hole flat without
touching the surface around it.
