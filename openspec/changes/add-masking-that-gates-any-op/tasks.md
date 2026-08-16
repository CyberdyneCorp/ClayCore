# Tasks: add-masking-that-gates-any-op

- [ ] 1.1 DECIDE where a gate attaches: item, node, or layer (proposal recommends ITEM as the smallest thing that answers the motivating case and composes)
- [x] 1.2 Bake a painted `MaskField` into a `FieldVolume` as the SIGNED DISTANCE to the
      protected region — NOT the paint values, which a narrow-band distance volume cannot
      carry faithfully.
      — ALREADY DONE: `brush::mask_to_field` (and `clay_mask_to_field`) is exactly this
      conversion, built for `mask_extrude`. Nothing new was needed. Three properties a gate
      leans on were untested and now are: the threshold decides what counts as masked, two
      disjoint painted regions both measure as inside with the gap between them outside, and
      the same mask measures identically after a round trip.
- [ ] 1.2b Caching against the mask's revision, so painting does not rebake per edit —
      still open, and only worth doing once the gate makes it hot
- [x] 1.2c DECIDE the protected-region threshold: fixed at 0.5, or exposed
      — DECIDED by what already exists: `mask_to_field` takes it as a parameter defaulting
      to 0.5, and a mask painted at partial strength everywhere has no clean boundary
      without one. The gate passes it through rather than assuming.
- [ ] 1.3 A gated combine in the kernel dialect: `mix(combine(acc, item, op), acc, mask)`, exact at both ends
      — a gate composes with EVERY mode rather than being one, so it belongs on the combine
      record and not in the mode enum. The record's shared prefix is four params with
      mode-specific extras after them, so the gate handle extends that prefix; `CLAY_OFF(pr, 4)`
      readers move with it. That is an encoding change, which is what
      `clay_tape_encoding_version()` exists to signal — the DOCUMENT format is untouched.
- [ ] 1.4 The exactness rule, following `cfi_transition`: the mix costs `|d_a - d_b| · Lipschitz(mask)`, and a uniform mask costs nothing
- [ ] 1.5 Scene-side attachment and serialization; an ungated document's bytes unchanged
- [ ] 1.6 Confirm per-brick culling needs no change — a gate only removes effect, so the influence bound stays a superset. Test it rather than assert it
- [ ] 1.7 C ABI and pyclay surface
- [ ] 1.8 Tests: a fully protected region is EXACTLY unchanged by a subtract; an absent mask is EXACTLY the unmasked result; a uniform mask costs no step scale; raymarching a gated document does not overshoot; the same painted mask protects both representations; an ungated document is byte-identical
- [ ] 1.9 Dialect parity across CPU, Metal, CUDA, OpenCL and Vulkan
- [ ] 1.10 Example with a render — the motivating case is a boolean that respects a mask, which is the picture worth showing
- [ ] 1.11 Docs: `sculpt_comparison.md`'s Tier 1 row, and the masking note in `voxel/mask.h` that currently states the authoring-only rule
