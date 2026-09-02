# Proposal: a flatten dab should cost what it touches

## Why

#272 made a relax dab cost what it moves. Flatten never got the treatment, and
it could not get the same one: `rewrite_region` preserves sparse support, and
flatten moves the surface by many band widths, so the facet lands in bricks
that hold no samples.

`field::flatten(const FieldVolume&, ...)` therefore still resamples
`v.bounds()`. Measured — one ball at the origin plus N-1 unrelated balls
marching off in +x, the same five-cell dab on the first ball's north pole in
every row:

| cell 0.02 | stored bricks | `flatten` | `relax`, same dab |
|---:|---:|---:|---:|
| 1 ball | 444 | 42.4 ms | 0.58 ms |
| 2 balls | 887 | 73.6 ms | 0.70 ms |
| 4 balls | 1,773 | 140.3 ms | 0.94 ms |
| 8 balls | 3,545 | **278.8 ms** | 1.53 ms |

At cell 0.015 the eight-ball row is **617 ms**. The interactive budget is 4.17.

The second cost is worse, and is not a latency problem. The overload samples
`v.eval(p)`, which outside the band returns the far bound — a value that steps
by BRICK, not by cell. With no flatten applied at all, just the resample the
overload performs:

| | stored bricks | declared `sample_lipschitz` |
|---|---:|---:|
| exact source | 444 | 1.00 |
| re-baked from `v.eval()` | 1,246 (**2.81x**) | **14.33** |
| re-baked twice | 2,088 (4.70x) | 14.33 |

Every in-place flatten inflates the volume ~2.8x and declares a Lipschitz of
14, which is a 14x step-count penalty on every later march of that layer, and
it compounds per stroke. #300.

## What

**`FieldVolume::resample_region(region, fill)`** — the primitive
`rewrite_region` could not be. It re-evaluates and RECLASSIFIES the bricks that
meet a region, so a brick may become stored, stop being stored, or change its
samples, while every brick outside the region keeps its bytes.

The region is the same one relax uses and no larger: the ball where the
operator's weight can be non-zero. Flatten changes the field only where its
weight is non-zero, so the new facet is inside that ball by construction — the
plane-slab and displacement margins the issue sketched are not needed.

**Flatten's fill prefers the volume's own stored sample** to `eval()`, and
falls back to `eval()` only where no brick stores one. That is what makes the
fill exactly the identity outside the region — bit-identical, not
approximately — which is what the halo invariant requires, and it is also what
keeps an untouched brick's exact samples out of the far bounds.

**The duplicate Lipschitz sweep goes.** `sample_blocks` already ends with
`sample_lipschitz_ = measure_sample_lipschitz()`, and `sample()` routes through
it, so both `flatten` sampling overloads measured the same unchanged samples a
second time.

## Impact

x86-64 release, load steady at 1.5 before and after each run, best of 3.

| a five-cell dab, one ball at the origin plus N-1 far away | before | after | |
|---|---:|---:|---|
| cell 0.02, 1 ball | 40.8 ms | **0.765 ms** | 53x |
| cell 0.02, 8 balls | 266.9 ms | 5.94 ms | 45x |
| cell 0.015, 1 ball | 93.6 ms | 1.73 ms | 54x |
| cell 0.015, 8 balls | 628.9 ms | 12.03 ms | 52x |
| cell 0.01, 1 ball | 262.7 ms | **3.48 ms** | 75x |
| cell 0.01, 8 balls | 1800.3 ms | 28.66 ms | 63x |

The scaling law is the point, and it is now on the term that matters: the dab
asks for **36 bricks** whatever surrounds them. What is left growing with the
model is the compact rebuild and the far-bounds chamfer, not the field
evaluation — at cell 0.01 with eight balls, 11.2 ms of the 28.7 is the volume
copy the overload returns, and most of the remaining 17.4 is the rebuild and the
chamfer against 36 bricks of actual evaluation. Those are the two terms the
non-goals below name, and they are ~60x cheaper per stored brick than the SDF
evaluation they replaced.

The scaling test is deterministic rather than timed: `resample_region` reports
how many bricks it evaluated, and the count does not move when unrelated model
is added.

Inflation, on a ball of 444 bricks at cell 0.02 declaring a Lipschitz of 1:

| | stored bricks | declared Lipschitz |
|---|---:|---:|
| the old whole-bounds resample, **no flatten at all** | 1,246 | 14.33 |
| one dab | **444** | 1.74 |
| eight dabs | **444** | 2.42 |

The 1.74 is the taper honestly steepening the field, which is what the declared
bound is for. The 14.33 was the far bounds being re-recorded as samples.

## Non-goals

**The document-sourced path.** `clay_item_volume_flatten_from` already takes
`region_min`/`region_max`, so a host can bound it today; it builds a new volume
rather than resampling one, so `resample_region` does not apply to it.

**Incremental far bounds and the compact rebuild.** Both stay global. #278
measured the chamfer at 0.571 ms against the 42-617 ms this is about; a global
`data_` rebuild is an O(stored) memcpy against an O(stored) SDF evaluation.
Optimising them first would be optimising the wrong term.

**A mutable sparse allocator, a stroke session, GPU flatten, SIMD.** #300 lists
these; none is needed to fix the scaling law.
