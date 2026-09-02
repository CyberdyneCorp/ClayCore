# Tasks

- [x] 1.1 Sweep the minimal sufficient pad across lengths 75-8000, three profiles, four chain orders, mixed/subtract compositions, three seed draws, arm64 and x86-64
- [x] 1.2 Fit the per-profile envelope holding >= 0.5k above every measured knee; clamp at support
- [x] 1.3 Keep `CullPadTerms` raw (largest k per profile) and resolve at read time, so the raise-only append stays exact
- [x] 1.4 Refute the node-map N with symmetry: sector-confined radial configs measure real disagreements at C = 8..64
- [x] 1.5 State the emitted multiplicity from `emit_item`: additive, `1 + popcount(mirror_axes) + (radial_count - 1)`, base-item copies only
- [x] 1.6 Re-knee the amplified family; confirm amplified knees match plain chains of the same effective length, combined modes included
- [x] 1.7 `layer_symmetry_multiplicity`, read live at every resolve site (blend_cull_pad, cull_pad, CullIndex::refresh_pad)
- [x] 1.8 Fold the seam k as its own raw maximum, resolved quadratic and clamped at the item-derived ceiling (the pre-#335 pad)
- [x] 1.9 Verify a symmetry edit can never reach a live index: not a tail append, so it takes the rebuild
- [x] 1.10 Pin the refuted configs live against the pre-#335 pad control; verify the pins FAIL with the multiplicity removed and with the seam fold removed
- [x] 1.11 Full bench gate; non-symmetric cull fixtures unchanged from the envelope-only pad
