# Design

## Why a caller may state its own region, and when it may not

`apply_edit` DERIVES each command's reach from the geometry, which is always
right and costs two influence bounds. A gesture that knows its reach
analytically can state it once for many commands — but a stated region that
does not cover the change is stale bricks, which is a wrong picture rather than
a slow one. So this is deliberately not a general facility:

- `GestureRegion` takes the region UP FRONT rather than accumulating it,
  because accumulating it is exactly what costs.
- It is RAII, because the invalidation must happen on the failure path too: a
  gesture that applied three commands and then refused has still changed the
  document.
- `apply_edit` remains the default. An entry point that cannot state its reach
  must keep the derived one, exactly as an entry point that cannot prove an
  append must keep the general invalidation.

## Why the drag's ball is a superset

The move warp displaces `p` by `d * w(p)`, and `w` is zero outside `radius` of
the centre. A point with zero weight is not moved, so the field there evaluates
the same nodes at the same place and cannot have changed. The changed region is
therefore inside the ball, whatever the drag touched and however many nodes it
warped.

Two things that might widen it, and do not: `front_only` only gates the pull
further, and an item's scale is uniform by design so the falloff stays
spherical mapped into the item's frame (`brush/move.h`). The displacement is
added as margin rather than because the argument needs it.

The radius is read through `read_desc`, not off `params->radius`: a caller may
pass an older struct version that does not carry the field.

## What proves it is not too tight

The oracle the resumed-refill tests already use: a FRESH document holding the
same items, dragged the same way, has no seeds, so its refill is the full
evaluation. Bit-for-bit, because a served seed is the same instructions over
the same floats.

Three shapes are covered — a drag across the seeded bricks, a drag repeated so
the second starts from seeds the first wrote, and a drag whose ball only clips
the brick row — plus the converse, that a far drag leaves those seeds alone.
Each asserts the drag actually moved the field those bricks read, since
otherwise the comparison is two readings of an unchanged field agreeing.

Mutation-tested: dropping the radius from the dilation, leaving only the
displacement, fails all three.

## The same shape, not fixed here

`clay_layer_lattice_gizmo` issues one `SetDeformersCmd` per warped node inside
one undo group, exactly as the move did, and pays the same per-command bound.
Its reach is knowable too — the cage, dilated by the largest offset.

It is NOT changed here, because no benchmark or device case covers it, and a
semantic change to an invalidation path with no measurement behind it is how
this defect arrived in the first place. Recorded so it is found rather than
rediscovered.
