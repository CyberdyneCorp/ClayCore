# Proposal: grow the chain pad with the chain, clamped at support

## Why

Issue #335, the pad half. `a-hard-blend-reaches-nowhere` removed the `k` a
hard profile never reads; what remained is that the pad a smooth chain DOES
need was spelled `max(support, k)` per item — for a quadratic sculpt blend,
`4k` added to every brick's cull region — and at an ordinary blend radius that
roughly doubles the survivor count per brick. A first cut capped the pad at a
flat `3k` and a sweep refuted it: the sufficient pad grows with chain length
(quadratic knees 2.30k at 75 nodes through 3.90k at 5000). So the pad now
states the measurement: `min(support, k * envelope(N))` per item, with a
measured per-profile log2 fit, clamped at the profile's support — the
pre-#335 pad — so it is never wider than what shipped, anywhere.

A second refutation then corrected what N is. The first envelope read the
layer's node-map size, and a layer's SYMMETRY multiplies the chain the
compiler actually emits: `emit_item` compiles a mirrored item once per copy —
`1 + popcount(mirror_axes) + (radial_count - 1)` instances, each folded into
the layer's one serial chain through its own seam combine. A 75-node map
under radial count 64 is a ~4800-contributor chain, and resolving the
envelope at 75 left real in-band disagreements: 15 sector-confined radial
configs (C = 8..64) measured up to 1.9e-3, 27x the fp16 floor bricks store
through, against zero for the pre-#335 pad. N is therefore the EFFECTIVE
contributor count — map size times the symmetry multiplicity — and the knee
campaign over the amplified family confirms amplified chains knee where plain
chains of the same effective length do, additive composition included.

The seam blends those copies enter through are the layer's `mirror_k` /
`radial_k` — quadratic, and fully independent of any item k. An item chain at
`k = 0.04` under a `0.12` seam kneed at 5.0 item-k, past what any item term
can say. The seam k folds in as its own raw maximum, resolved as a quadratic
chain term and clamped at the ceiling the ITEM blends alone resolve to, which
IS the pre-#335 pad — so a wide seam pads the chain without ever exceeding
what shipped, and where it wants more the cull is bit-identical to main.

## What Changes

- `chain_pad_envelope(profile, n)`: the measured fit, in k-multiples —
  quadratic `2.80 + 0.35*log2(n/75)`, cubic `2.75 + 0.50*log2(n/75)`,
  circular `2.70 + 0.30*log2(n/75)`, floored at the 75-contributor base.
- `layer_symmetry_multiplicity(layer)`: `1 + popcount(mirror_axes) +
  max(0, radial_count - 1)`, the conservative per-item instance count.
- `CullPadTerms` holds RAW maxima — largest item k per measured profile, and
  the largest seam k in a NEW slot so the item slots stay pure and the
  pre-#335 pad remains derivable from them as the seam's ceiling.
  `blend_total(n_eff)` resolves everything at READ time against the effective
  count, which keeps `CullIndex`'s raise-only append exact: raw maxima, the
  node count and the live layer's multiplicity only rise under append, and a
  symmetry edit is never an append — it takes the general invalidation and
  the rebuild with it.

## Impact

- The pad shrinks only where the envelope sits under the support; wherever
  the clamp binds the tape is bit-identical to the pre-#335 compile. Bar
  held: equal-or-fewer band-clamped in-band disagreements than the shipped
  `max(support, k)` pad, per config, live-controlled in the pin suite.
- Cull benches at N <= 600 measure ~11.5% fewer instructions; clamped
  configs are identical.
- No bound moves: geometry and influence bounds are untouched, exactly as
  `a-hard-blend-reaches-nowhere` required.

## Non-goals

- A proof for arbitrary chains. The drag grows with length and no fixed
  dilation bounds it; the support clamp, not the fit, is the last word, and
  the envelope may not be lowered without the sweep's breadth of evidence.
- Counting symmetry exactly. The multiplicity over-counts groups, invisible
  nodes and items that opted out of the mirror; every term stays clamped at
  its own support, so over-counting only costs pad width it already had.
