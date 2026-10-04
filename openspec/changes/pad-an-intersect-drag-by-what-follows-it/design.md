## Context
`item_geometry_reach_in_document` is #471's proof that moving an intersect operand cannot change the band-clamped field outside the swept union of where it was and where it went. Outside that union the operand is beyond the band on both sides, so `max(acc, item)` is too, but the RAW value differs. A smooth combine further down the chain can read that difference and carry it back into the band. The chain pad is the term for that, and it was `cull_pad(content, layer)`: the largest blend in the layer at the cull's chain envelope.

## Decisions

### 1. Only the combines after the operand are terms
The sequence of combines that reads the running value the move changed is: the operand's later siblings, then the enclosing group's own combine, then that group's later siblings, and so on up to the layer root. An inline group's children continue the outer chain (`compile_group` returns `compile_list(children, ..., have_acc)`), so they are in the sequence. A non-inline group's children start a chain of their own and are not; the group's own combine is. The enclosing groups' combines were already applied by `dilate_by_ancestors` at full support, and stay there.

The operand's own combine and seam are not terms. Its geometry bound carries `max(support, k)` of its own combine and the seam's support around its copies (`placed_local_bound`). A smooth max only raises, and the seam's union lowers by less than its support. The triage plan kept them as terms for safety. The sum argument below does not need them, and leaving them out is what makes an operand appended last carry no pad.

### 2. A sum of full supports, not the cull's envelope
The plan was to keep `cull_pad`'s envelope over the suffix. Building the oracle fixtures refuted it (proposal, "What building it found"). The chain drags its running value down a little per smooth step, and with 96 smooth stamps after the operand the accumulated drag reached in-band stamps 0.3 past the box. Neither the envelope nor the largest support covered it.

Write `T_j = band + R_j`, where `R_j` is the sum of the supports of the combines after combine `j`. Take a changed running value `a > T_j + s_j` entering combine `j` of support `s_j`:

- `|a - b| >= s_j`: the combine is its hard counterpart, bit for bit. It returns `b` (unchanged on both sides) or `a` (still above `T_j`).
- smooth union, `|a - b| < s_j`: then `b > a - s_j > T_j`. The result is at least `min(a, b) - dip * h^2` with `h <= (b - T_j) / s_j` and `dip < s_j`, so it stays above `T_j`.
- smooth subtract or intersect: the result is at least `max(...)`, which is at least `a`.
- feathered replace: `a + w * clamp(b - a, -band_v, band_v)` moves `a` by at most `band_v`, the feather term the carry counts.

By induction every changed value stays beyond the band through the last combine, so the band-clamped value a brick stores is bit-identical. `combine_carry` is `cull_pad_terms(node).support_total()` (full support per profile, the feather, the seam), widened by `chain_blend_support` for an extended mode whose reach is its rounding.

### 3. Clip in the C ABI, not in the scene query
The sum is exact and, for an operand ahead of a long smooth chain, large: 120 stamps of k 0.06 come to 28.8. `apply_edit` already holds the conservative influence union for both sides, so it reports the overlap of the two. Each region holds the whole change, so their overlap does too. `command_surface_delta_bound` stays the unclipped proof, so its tests can pin the arithmetic. If the overlap were ever empty, the delta is reported instead: two sound regions can only disagree that way when nothing changed, and over-dirtying is the safe direction.

### Alternatives rejected
- **Refuse the delta path when any smooth combine follows the operand.** Exact, and simpler to state. But it throws away a small box where one or two smooth combines follow, which the sum keeps: the "smooth combine downstream" fixture's box is the sweep plus 1.0.
- **Per-brick refinement** (the issue's option 2). The appended-last case, which is the host's, needs none once the pad is right. Revisit only if a measurement asks for it on a mid-chain operand.
