## Why

Issue #306, as it now stands. The resumable brick refill made a dab INSIDE a
stroke flat in document size — a brick that has been filled before evaluates
only what the document gained since. What is left is the brick that has never
been filled:

| items | first touch of a window, no seed | the same window warm |
|---:|---:|---:|
| 5,000 | 3.2 ms | 0.004 ms |
| 20,000 | 12.9 ms | 0.006 ms |
| 50,000 | **37.8 ms** | **0.004 ms** |

A SECOND cold window costs the same as the first (33.7 ms against 37.8), so it
is the walk and not the index build. A stroke crosses brick planes constantly,
so this is a hitch in the middle of a gesture rather than a one-off.

The issue names the fix itself: "a coarser resident prefix — one level of a mip,
or a low-resolution `FieldVolume` over the layer, accurate enough to seed a
brick and refined by the suffix … That argument is unchanged and unused."

`SdfPrefixCache` **is** that, and has been since ABI 0.79.0. It was reachable
only from the Smooth transaction.

## What Changes

- `clay_brick_cache_eval_requests_seeded` — the refill, given a prefix cache. A
  brick with no seed at all is seeded from the layer's cached prefix (roots
  [0, K) sampled out of a volume) and evaluates only roots [K, end) through the
  `plan_frontier` machinery the frontier path already uses. The result is stored
  as an ordinary seed, so the SECOND touch takes the existing warm path.
- `clay_sdf_prefix_cache_build_for_refill` — the same build, on the lattice a
  refill reads.
- The cache learns two things it did not need when Smooth was its only consumer:
  a lattice-alignment choice, and a way to not re-prove itself on every call.

## The three things building it found

**A seed is only exact on the lattice it was built for.** Smooth's working field
is the layer's padded region, so its prefix shares a lattice with it by
construction. A refill's lattice is the brick grid, anchored at the world origin.
Read across the two, a seed is an interpolation of two samples rather than one of
them — **0.011 at a 0.05 cell, a quarter of a cell**, against 3.1e-07 aligned.
Snapping the region unconditionally was tried and is wrong: it moves Smooth's
prefix off Smooth's lattice, which `test_sdf_prefix_cache.cpp`'s "exact on the
lattice it was built for" catches at 0.0149 against its 1e-5 gate. So alignment
is a POLICY choice and part of the cache key, and the two consumers hold separate
entries.

**Proving a prefix still valid is O(prefix roots), and that is the wrong cost per
frame.** `find` re-digests the prefix's roots as a safety net against a missed
invalidation — 13.5 ms at 50,000 roots, measured on its own. That is right for a
Smooth transaction, which asks once per gesture, and wrong for a refill, which
asks every frame. The digest stays; a caller may now hand in a monotonic witness
of structural change, and it is recomputed only when that moves. `clay_c`'s
`structure_revision_` is such a witness: an append cannot change roots before the
boundary.

**The policy's boundary moves with every stamp.** `prefix_boundary_for` is
`roots - keep_live_suffix_roots`, so a lookup by the current boundary misses on
every dab of a stroke — exactly when a cold window is reached. An older prefix is
still valid, since an append cannot change the roots before it, so the lookup
takes the best boundary the cache holds and the suffix absorbs the difference.
Gated: 12 of 12 bricks still seeded after 12 stamps.

## Result

A cold window, on a document that has never been refilled there:

| items | unseeded | seeded |
|---:|---:|---:|
| 5,000 | 1.69 ms | **0.271 ms** |
| 20,000 | 7.68 ms | **0.290 ms** |
| 50,000 | 14.65 ms | **0.291 ms** |

**Flat.** 0.271 to 0.291 ms across a ten-fold document, and **50x** at 50,000
items. Exact to 4.5e-07 over 26,245 in-band samples, with 94-95 of 96 bricks
served by the prefix.

Held by `BM_ColdWindowSeeded` against `BM_ColdWindowSeededSmall` -- the same
window on a document four times larger, 0.304 against 0.307 ms -- because
flatness is the claim and a ratio against the unseeded row cannot make it.

**This closes #306.**

### The residual, and what it turned out to be

The first version of this measured 2.5x and stayed linear, and the cause was
guessed twice before it was measured. It was neither the validity digest (which
the witness had already memoised) nor the suffix length (varying it from 4 to
256 roots moved nothing). Phase timers put 3.5 ms of a 5.4 ms window in the
suffix COMPILE, and the reason is one branch:

```
if (cull && index)   pad = index->cull_pad();
else if (cull)       for (const Layer& l : doc.layers) pad = max(pad, cull_pad(...));
```

`compile_layer_suffix` given a cull region but no index re-derives the
document's cull pad by walking every layer's items -- **per brick**. The cold
path had been written to skip taking the cull index, on the reasoning that
`plan_frontier` does not need one and the copy is what the branch already
declines to pay. It does not need one; the per-brick suffix compile underneath
it does. Taking it took the released phase from **3,500 us to 24 us**.

## Capabilities

### Modified Capabilities
- `c-abi`: a refill can be given a prefix to seed cold bricks from.
- `brick-cache`: what a cold brick may start from, and when it may not.

## Impact

- `include/clay/session/sdf_prefix_cache.h`, `src/session/sdf_prefix_cache.cpp`.
- `bindings/c/clay.h`, `clay_c.cpp`.
- **ABI 0.83.0 -> 0.84.0.** No existing signature changes; the default path is
  byte-identical and pays nothing.
