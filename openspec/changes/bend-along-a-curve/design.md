# Design — bend along a curve

## The map, and why it is the sweep read backwards

A claycore deformer is an **inverse point map**: the evaluator hands it a point
in deformed space and it returns where to sample the undeformed field. So the
implementation is not "bend the item along the curve", it is "given a bent
point, undo the bend".

`Prim::swept` already computes, for any point, its nearest point on a guide, the
arc length there, and a parallel-transported frame. That is precisely the data
the inverse bend needs:

```
hit    = csweep_nearest(guide, count, p)      // segment + parameter
frame  = lerp of the two transported normals, re-orthogonalized     (as ctape_swept)
s      = arc length at the hit
perp   = p - guide(s), minus its tangential part
return (t0 + (s / total) * (t1 - t0),   dot(perp, normal),   dot(perp, binormal))
```

with the axis coordinate first when the axis is X. The item's straight span
`[t0, t1]` is what gets laid onto the guide.

Reading it backwards is not a trick — a sweep and a bend-along-a-curve **are**
the same geometry seen from either end, and the code says so by sharing the
query rather than by a comment claiming it.

### A straight guide is the identity

With a guide running straight along X from `(t0,0,0)` to `(t1,0,0)`, the
transported normal is constant, `binormal = cross(X, normal)`, `s = p.x - t0`,
and the map returns `p` unchanged. This is the same shape of claim stage 1 made
about ranged twist reducing to unranged twist, and for the same reason: it is
what makes `bend_curve` a **generalization of** the straight item rather than a
second thing that has to be kept in step. It is asserted in the tests.

A guide that is a circular arc reproduces `bend` to within the guide's own
tessellation, which is asserted too — with a stated tolerance, since a polyline
is not an arc.

## Where the guide lives

A deformer record is 12 floats (`type, k, a, b, c, ease, ext[6]`). A guide is
not a fixed size. Three options were on the table:

1. **Reuse the node's `stroke` field.** Free, and wrong: a node whose *primitive*
   is a stroke or a sweep already uses it, so this would silently forbid
   bend-curving the exact items most likely to want it.
2. **Widen the deformer record.** Every deformer on every item pays for the
   widest one, and the record is read in the innermost loop of every backend.
3. **Put the guide in the blob and the offset in the record.** What `ctape_swept`
   already does, with the same 7-float vertex layout.

(3), and it is the reason this change is worth its size. The cost is that
`ctape_deform_point` must take the blob pointer:

```c
CLAY_FN cfloat3 ctape_deform_point(CLAY_FPTR rec, CLAY_FPTR blob, cfloat3 p)
```

The blob is already in hand at the only call site (`ctape_prim_local` receives
it), so this is a threading change rather than a plumbing one — but it is a
**kernel dialect signature**, so it compiles five ways and the dialect gate
checks all five. Paying it here also pays it for the Lattice, whose cage has the
same shape of problem and would otherwise force the same change later against a
larger surface.

## The bound

`cfi_bend_curve` charges two things, mirroring `cfi_swept`:

- **Curvature.** On the inside of a bend of radius `R`, a point at perpendicular
  offset `r` is compressed by `R / (R - r)`. When the item's cross-section
  reaches the tightest bend the map folds through itself; as everywhere else in
  this file that **degrades to a very small step rather than being refused**,
  because a guide is editable after the fact.
- **Axial rescale.** The span `[t0, t1]` is laid onto `total` arc length, so the
  axis direction is scaled by `span / total`. Only a value above 1 costs
  anything — a guide longer than the span contracts, and contraction is safe —
  so the term is `max(1, span / total)`.

The tightest bend radius is the **circumradius** of consecutive vertex triples,
not the accumulated turn angle. This matters and the sweep already learned it:
an angle estimate reads a finely-tessellated gentle curve as a tight one,
because short segments accumulate angle. That function is factored out of
`swept_field_info` rather than written twice, so the two cannot drift.

## The hull

Unlike twist and bend, the warp is **not** contained by the undeformed item's
own neighbourhood — a curve can carry material anywhere the guide goes. The
influence bound is therefore the guide's own AABB grown by the item's
cross-section extent, which is finite and, unlike the arc-based hulls, tight.
