# Design

## The region needs no margin for how far the surface moves

#300 sketched a selector built from the brush ball, the target plane's ±band
slab, and a displacement allowance, on the grounds that flatten moves the
surface and the new facet has to be covered.

It does not need one. `flatten_at` returns `src(p)` unchanged wherever the
weight is zero, and `region_weight` is zero outside `region_radius + falloff`.
So the FIELD outside that ball is unchanged, so its ZERO SET outside that ball
is unchanged, and every surface flatten creates lies inside the ball that
created it. The selector is the same one relax uses:

```
Region::ball(settings.centre, region_radius + max(falloff, 1e-4))
```

`max(falloff, 1e-4)` rather than `falloff`, because that is the clamp
`region_weight` itself applies, and a region measured to a different taper than
the operator uses is the one failure mode this cannot detect.

What flatten needs that relax does not is not a bigger region. It is permission
to change WHICH BRICKS STORE SAMPLES inside it.

## `resample_region` against `rewrite_region`

```
rewrite_region()    values change, sparse support fixed         relax, filtering
resample_region()   values change, sparse support re-decided    flatten, displacement
```

`resample_region` takes a `BrickBlockFill` — the same contract `sample_blocks`
takes — evaluates it for each selected brick, classifies the result with the
same `scan_block` the bake uses, and rebuilds. There is deliberately no second
definition of what "near the surface" means.

Classification happens AFTER the fill, which is what lets a brick that stored
nothing store the facet. It is also why no pre-operation cull may be introduced
between the two: a brick that looks irrelevant against the source surface is
exactly where the facet lands.

### Windows of one

`sample_blocks` hands the fill consecutive slot windows of 512. A region
selects a scattered set, so `resample_region` calls the fill once per brick,
`count == 1`. For a few dozen bricks the `std::function` call is not
measurable, and the alternative — coalescing runs along x so a tape evaluator
could cull once per window — is worth doing when a tape evaluator is the fill.
Today's caller reads a volume.

## Why the fill prefers the stored sample

`rewrite_region`'s precondition is "`fn` is the identity outside the region",
and it is load-bearing twice: skipped bricks keep old values, and a sample on a
brick face is stored by every brick sharing it, so changing one copy while
another sharer goes unselected steps the field at the face.

`resample_region` needs the same guarantee, but its fill produces values from
scratch rather than transforming what it was handed, so "identity" has to mean
"reproduces what is stored". Flatten's fill gets that exactly, not
approximately:

```
source(p) = v.sample_at(cell of p)  if some brick stores it
            v.eval(p)               otherwise
```

At weight zero `flatten_at` returns `source(p)` untouched, so a sample in a
stored brick comes back as the identical float. That covers the shared-sample
case completely, including the interesting one — a brick that was EMPTY and is
becoming stored, whose face samples belong to a stored neighbour that was not
selected. It reads that neighbour's stored copy, so the two agree by
construction rather than by float luck.

Trusting `v.eval(p) == stored sample` at a sample position instead would be
relying on `(origin + n*cell - origin) / cell` landing exactly on `n`. It very
nearly always does. "Very nearly" is not what a halo invariant can rest on.

Preferring the stored sample is also the substantive fix for the 2.8x brick
inflation. `eval()` away from the band returns the far bound, which steps by
brick; re-recording those steps as samples is what declared a Lipschitz of 14
on a field whose source declared 1. Now only a brick with no stored sample at
all can contribute one, and only inside the brush.

## The fallback is floored at the band, or the shell grows back

Preferring the stored sample is only half of it. Where NO brick stores a sample,
the fill still has to produce one, and `eval()` there returns the far bound —
which is deliberately smaller than the truth. `far_value()` is
`band - 0.87 * cell`: it has to bound the points BETWEEN samples, which can be
up to half a cell diagonal nearer the surface than the nearest sample is.

So a sample-free brick next to a stored one reports a value INSIDE the band, and
re-evaluating it stores the brick. That is the shell growth `compact()` exists
to undo after a bake, and it arrived here as a Lipschitz of 11.5 on a dab that
should have declared under 2 — one new brick full of brick-sized steps is enough
to set a number that is read for the whole layer. It also made a fully masked
flatten stop being the identity, because the frozen brush still created bricks.

The fix is to hand back what the volume actually guarantees rather than what it
reports. A brick is skipped only when every SAMPLE in it lies beyond the band,
and a sample with no stored copy is one that every brick sharing it has said
that about. So the bound is floored at the band — just past it, since the
near-surface test is inclusive and a sample sitting exactly on the band would
store the brick the floor exists to keep empty.

That is a tightening of the volume's own knowledge, not a fabrication: `eval()`
is a lower bound on the magnitude, and the sparse index is a second one. Taking
the larger of the two is what both of them together say.

## Lipschitz

Bricks the resample did not touch cannot have got steeper, so the volume's own
declaration bounds them. `resample_region` measures only the blocks it wrote
and keeps the old declaration as a floor:

```
sample_lipschitz_ = max(sample_lipschitz_, steepest over the blocks written)
```

This may OVERSTATE — if the steepest brick in the old volume is one the
resample emptied, the floor outlives the samples that set it. Overstating a
Lipschitz bound is the safe direction: it shortens the marcher's step. Flatten
already applied exactly this floor for exactly this reason.

`measure_sample_lipschitz` and the resample share `steepest_in_block`, so the
two cannot come to different answers about the same block.

## Far bounds are rebuilt globally

`build_far_bounds` is a two-pass chamfer over every brick SLOT, and what it
computes depends on which bricks store samples — which the resample has just
changed. #278 measured it at 0.571 ms against the 42-617 ms this change is
about. Rebuilding it globally is the right first trade, and its incremental
form is not byte-identical to the global one, which is an argument that
deserves its own change.

`far_` entries for untouched bricks keep their SIGN, which is all
`build_far_bounds` reads back; only the magnitude is re-derived.

## The compact rebuild

`data_` is compact — each stored brick owns a contiguous `kBrickSamples` block
and `index_[slot]` points into it — so bricks appearing and disappearing cannot
be patched in place. The rebuild walks slots in order once, copying an
untouched brick's block and inserting a written one's, which is an O(stored)
memcpy against the O(stored) SDF evaluation it replaces.

Selected slots come out of the `bz/by/bx` loop in ascending slot order, so the
rebuild merge-walks two sorted sequences rather than needing a lookup
structure.

## `flatten(const FieldVolume&)` stops sampling

The overload becomes: copy the volume, resample the brush's ball into the copy,
return it. Reads come from the ORIGINAL, which is untouched, so there is no
half-written-input hazard and no snapshot needed — the same ordering rule
`snapshot_region` documents, satisfied by construction.

One behaviour change beyond the intended one: when `resolve_plane` refuses the
settings — no plane, no strength, or no region — the overload used to return
`FieldVolume::sample(v.eval, v.bounds(), ...)`, a resampled copy carrying the
inflation above. It now returns `v`. "Sample the source unchanged" is what that
branch means, and `v` IS the source, sampled.

## Transient memory

`resample_region` holds the fill's output for every selected brick before it
rebuilds, and reserves the rebuilt `data_` alongside the old one — so a region
covering the WHOLE volume peaks at roughly three copies of the samples. For a
brush that is a few dozen bricks against a volume of thousands, which is what
this exists for, the first term is nothing and the second is the rebuild that
was always going to happen.

## What is not covered

The document-sourced path (`clay_item_volume_flatten_from`) builds a NEW volume
from a tape over a caller-supplied region. There is no volume to resample, so
`resample_region` does not apply, and the caller can already bound it. It keeps
`sample_blocks` with the blend inside the fill, which is the same
classify-after-blend rule stated once more in a place that had it first.
