# Tasks: add-masking-that-gates-any-op

- [x] 1.1 DECIDE where a gate attaches: item, node, or layer
      — DECIDED: the ITEM (`scene::Node::gate`). It is the smallest thing that answers the
      motivating case, and a layer-wide gate is the same gate repeated. Nothing forces the
      choice the other way, and starting narrow leaves room to add a wider one later.
- [x] 1.2 Bake a painted `MaskField` into a `FieldVolume` as the SIGNED DISTANCE to the
      protected region — NOT the paint values, which a narrow-band distance volume cannot
      carry faithfully.
      — ALREADY DONE: `brush::mask_to_field` (and `clay_mask_to_field`) is exactly this
      conversion, built for `mask_extrude`. Nothing new was needed. Three properties a gate
      leans on were untested and now are: the threshold decides what counts as masked, two
      disjoint painted regions both measure as inside with the gap between them outside, and
      the same mask measures identically after a round trip.
- [x] 1.2b Caching against the mask's revision, so painting does not rebake per edit
      — DONE, and the note above undersold it. It said a static gate "already costs
      nothing per frame", which is true of EVALUATION and not of the bake: `gate()`
      measures the mask on every call, and measured through the binding that is 21 ms at
      four thousand painted cells and 145 ms at thirty thousand. Gating fifty items by one
      painted mask paid it fifty times — 1.0 s and 7.2 s.

      `voxel::MaskField` now carries a change token bumped by every mutator, and
      `brush::GateBake` memoises one bake beside a mask handle. Fifty gates on an
      unchanged mask: 1050 ms -> 22 ms and 7250 ms -> 149 ms, about 48x, and the fifty
      items now SHARE one volume instead of holding fifty. A repaint still pays the full
      bake, which is the point rather than a shortfall.

      The band rule (`band = 2 * width`) moved into `GateBake` on the way: it had been
      spelled out identically in both bindings, and an arithmetic invariant the gate's
      correctness depends on should not exist twice.
- [x] 1.2c DECIDE the protected-region threshold: fixed at 0.5, or exposed
      — DECIDED by what already exists: `mask_to_field` takes it as a parameter defaulting
      to 0.5, and a mask painted at partial strength everywhere has no clean boundary
      without one. The gate passes it through rather than assuming.
- [x] 1.3 A gated combine in the kernel dialect: `mix(combine(acc, item, op), acc, mask)`, exact at both ends
      — a gate composes with EVERY mode rather than being one, so it belongs on the combine
      record and not in the mode enum. The record's shared prefix is four params with
      mode-specific extras after them, so the gate handle extends that prefix; `CLAY_OFF(pr, 4)`
      readers move with it. That is an encoding change, which is what
      `clay_tape_encoding_version()` exists to signal — the DOCUMENT format is untouched.
- [x] 1.4 The exactness rule, following `cfi_transition`: the mix costs
      `|d_a - d_b| · Lipschitz(mask)` — `cfi_gate`, with the weight's slope 1.5/width and
      the difference bounded by the ITEM's reach rather than the region's, since outside
      where the item acts the gated and ungated fields are the same field.
      — MEASURED, and worth stating plainly: a gate is NOT cheap. At width 0.05 it is a
      ~90x step-scale cost and at 0.80 still ~7x. That is the arithmetic rather than
      overhead, and the example prints the table rather than burying it.
- [x] 1.5 Scene-side attachment and serialization at minor 11; an ungated document's bytes
      unchanged, and writing AT an older minor drops the gate and keeps the item — a
      downgrade rather than a different document.
      — A latent test bug surfaced here: `test_volume_color` wrote at minor 8 and read at
      the DEFAULT minor, which only ever worked because every section added since lived
      inside the volume's own length-prefixed blob. The first outer section added after it
      broke the shortcut. Fixed to read at the minor it wrote.
- [x] 1.6 Confirm per-brick culling needs no change — tested, not asserted: the gated and ungated tape bounds match, and compiling against a cull region agrees with the unculled tape where the item still acts
- [x] 1.7 C ABI (`clay_item_set_gate`) and pyclay (`Prim.gate(mask, width, threshold)`),
      both measuring the mask rather than taking a volume — a host has a mask, not a field.
      Both derive the measured BAND from the width, because a band narrower than the fade
      means full protection is never reachable and the surface returns at ~92% of where it
      should be, which reads as a subtle bug rather than a misconfiguration.
- [x] 1.8 Tests, all of the above except the two-representation one (1.10 below).
      — The exactness claim CAUGHT A REAL DEFECT rather than confirming one: `cmix(x, y, t)`
      is `x + (y - x) * t`, so `t == 1` returns `y` only up to rounding. The fully-protected
      end was one ULP off, which is a one-float seam along the border of every protected
      region. The kernel branches there now, and the test walks the whole region rather than
      three points.
- [x] 1.9 Dialect parity across CPU, Metal, CUDA, OpenCL and Vulkan
- [x] 1.10 Example with a render: `examples/54_masked_operations.py` bores a bar through a
      ball and gates the subtract, so the hole comes back half filled in.
      — The equivalence between a masked VOXEL edit and the equivalent masked SDF operation
      (the voxel-engine delta's scenario) is not covered yet; the two mechanisms are
      independent today and the claim wants its own fixture.
- [x] 1.11 Docs: `sculpt_comparison.md`'s four masking rows, the evidence table, and the authoring-only note in `voxel/mask.h`
