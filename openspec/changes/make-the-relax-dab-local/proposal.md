# Proposal: a relax dab should cost what it moves

## Why

At a 0.01 cell, a **five-cell** brush cost **16.7 ms**, of which about 0.6 ms
was work inside the brush. The other 96% was `rewrite()` walking every stored
brick in the volume so the callback could pay `sample_at`, `cell_position` and
`region_weight` per sample and then return the sample unchanged.

The cost tracked the MODEL, not the dab:

| cell | stored samples | brush radius 0.05 | whole field |
|---|---:|---:|---:|
| 0.05 | 67,068 | 0.67 ms | 3.68 ms |
| 0.02 | 403,866 | 4.23 ms | 18.5 ms |
| 0.01 | 1,554,228 | **16.7 ms** | 71.5 ms |

Shrinking the brush from the whole field to five cells — a ~10,000× drop in
affected volume — bought 4.3×. The rest was a floor of
`1,554,228 stored samples x 10.3 ns`.

`openspec/changes/make-the-brush-cost-local` already named this disease on the
mesh side, and its title is the right one: a dab should cost what it moves.
This is the SDF instance.

## What

`FieldVolume::rewrite_region(region, fn)` walks only the bricks that meet the
region. `relax` derives that region from where its weight can be non-zero — the
brush radius plus the **taper**, which is the widened falloff rather than the
radius — and falls back to the full sweep when `region_radius` is zero, which
means "everywhere" and is a filter rather than a brush.

`fn` must be the identity outside `region`. That is a precondition, not a hint,
and `design.md` has why it is load-bearing in two separate ways.

## Impact

| cell 0.01, `radius_cells` 1, one pass | before | after | |
|---|---:|---:|---|
| brush radius 0.05 | 16.714 ms | 2.234 ms | **7.5×** |
| brush radius 0.20 | 18.613 ms | 5.881 ms | 3.2× |
| brush radius 0.40 | 23.498 ms | 13.680 ms | 1.7× |
| whole field | 71.543 ms | 73.329 ms | unchanged, as it must be |

Four passes at radius 0.20: 72.453 ms → 21.137 ms, **3.4×**.

The win scales with how local the brush is, which is the point: the cost now
follows the dab rather than the document.

## Non-goals

**The two remaining whole-volume terms** — `shrink_band`'s global far-bounds
chamfer (0.571 ms) and the per-pass volume copy (0.162 ms), together 33% of a
small dab at cell 0.01. Both are real, both now matter, and the local form of
the first is **not** byte-identical to the global rebuild, which is an argument
that deserves its own review rather than a paragraph at the end of this one.
#278.

**A brick-list overload.** `rewrite_bricks(span<BrickCoord>)` was in the
original sketch. An AABB is what relax actually has, the selection is
conservative anyway, and a list would only pay once there is a dirty set to
supply it — which is the live-session work, not this.

**Anything about the kernel.** Stencil caching, radius specialization, the
brick+halo scratchpad, ping-pong buffers and `std::function` removal were all
measured against the old code and are noise or nearly so. They should be
re-measured against the residual profile, not this one.
