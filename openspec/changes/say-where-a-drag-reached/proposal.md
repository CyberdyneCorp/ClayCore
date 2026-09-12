## Why

**`clay_layer_move_surface` reports a count and not a region, so a host must
over-estimate the very region that dominates its stroke cost.**

The engine already computes that region, and its version is better than
anything a caller can reconstruct. The call builds `std::vector<math::Aabb>
reach` — one box per drag image — and the comment beside it says why:

> Cheaper AND TIGHTER than what apply_edit would derive. Per command it unions
> the whole influence bound of each moved node, so a drag that catches the edge
> of 257 items spread through the volume invalidates the union of 257 whole
> items — far more than the ball the drag actually reached.

It was then passed to the invalidation and discarded.

ClaySpaceDesktop's `move_surface_stroke` computes its own box from
`brush.size + distance travelled` **because this call reports a count and not a
region** — a host reconstructing, less well, a number we already hold.

**Under symmetry the reconstruction is not merely loose, it is wrong.** A drag
states one box per image (#363 — a mirrored drag moves where the *reflected*
ball is; leaving it at one ball measured 997 of 8,192 samples stale, the whole
pull). A caller with one box either misses the reflected side, or unions them —
and this file already records what that costs: *"the union of two balls a
diameter apart is the slab between them, and under a mirror that slab is the
whole document."*

## Why this and not the bound work

Measured across #531:

| | |
|---|---:|
| host cost per press, refill + mesh | **6.5–36.7 ms** |
| host cost per press, the edit itself | 2–4.6 ms |
| the whole engine, live Move drag | 10.0 ms |

**Refill is the phase that costs, and its cost is set by how many bricks are
marked dirty.** A tighter region acts on it directly. #541 and #542 were real
defects and shipped, but a dense per-brick evaluation never reads
`safe_step_scale`, so neither could reach this.

## The refusal is free, and that is the design

The region set is final before the first edit is recorded: the dilation into
document space and the sharer loop are reads, `drag_frontier` takes a **const**
document, and `prepare_frontier_seeds` after it is the first thing that records
anything.

So the capacity check happens **there** — a caller with a short buffer is
refused with the needed count and **nothing is applied**. That is the same order
the call already uses for a protected layer, refused before the resampling
rather than after it. A partial fill that had already edited the document would
leave a caller unable to invalidate correctly, which is worse than refusing.

The check lives inside the shared implementation rather than being recomputed
by the new entry point, so it cannot drift from the set it is counting.

## What building it found

**The boxes worth reporting are not the ones the drag starts with.**
`apply_surface_gesture` dilates them into document space — a smooth fold above
the layer moves the document's surface further than the layer's own change —
and then widens them by the whole influence bound of every other layer sharing
the edit list. Reporting the raw balls would have handed a host a region that
looked precise and was short.

## What changes for a caller

Additive. `clay_layer_move_surface` is unchanged and now delegates to the shared
implementation; a test pins that it answers exactly what it did.
