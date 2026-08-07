# Tasks: add-surface-relief

- [x] 1.1 `ccombine_relief` and `ccombine_incise`: offset the accumulator by an
      amplitude, weighted by the item's own field as a region, following paint's
      precedent — two ops sharing one branch, see 1.9
- [x] 1.2 Finite support, so influence bounds and brick culling are unaffected
- [x] 1.3 `cfi_relief`: amplitude over falloff width
- [x] 1.4 Bounds: the surface can move by the amplitude, so the bound grows by it
- [x] 1.5 Scene op, C ABI and Python bindings, on the existing parameter convention
- [x] 1.6 A parity corpus row
- [x] 1.7 Tests: it builds up and cuts in, it contributes nothing alone, support is
      finite, zero is a no-op, the declared Lipschitz holds, a ray still lands,
      and it survives a round trip
- [x] 1.8 Docs, example, full verification

Found while building:

- [x] 1.9 Scoped as ONE op with a signed amplitude, arguing from magnify and
      pinch. Wrong precedent: a deformer carries parameters in a free-form float
      array where a sign is free, while an op carries `blend_k`, which is
      validated non-negative in three places including the blend constructor —
      which has no op to be aware of. A negative amplitude was rejected the
      moment it was tried. Split into Relief and Incise, sharing one kernel
      branch, which is also the convention already here: add/subtract and
      engrave/emboss are pairs.
- [x] 1.10 `op_is_extended` is a numeric RANGE, and the transitions sit between
      Replace and the new ops — so relief had to be added explicitly rather than
      by extending the range, or the non-local transitions would have been swept
      in and culling could drop them.
- [x] 1.11 The exactness needs the rounding in WORLD units, which `fold_info`
      did not receive. Using the local value would divide the amplitude by too
      large a width where an item is scaled down, understating the slope — the
      direction that makes a marcher overstep.
- [x] 1.12 The reach is region + ROUNDING + falloff, not region + falloff. The
      finite-support test asserted the shorter one and failed by 3e-4 at the rim.
      Not a defect: the rounding is the falloff width AND it rounds the region's
      own field, so the taper sits outside the rounded surface — the same double
      duty groove and tongue already have, and what the bounds already dilate by.
      The example measures the reach (0.519 against a predicted 0.52) rather
      than asserting it.
- [x] 1.13 The first crease render read as a raised ridge, not a groove. The
      field was right — measured -0.09 along the bar — but the camera was at a
      glancing angle and the lit far wall of the trough looked like a bump.
      Replaced with a before/after pair seen from above.
- [x] 1.14 Amplitude over falloff width is one number seen twice: it is what
      makes the rim a ledge rather than a swell, and it is exactly the slope
      `cfi_relief` declares. The example renders and prints it together.
- [x] 1.15 `fold_info` was 38 before this change and the relief branch made it
      39, over even the compiler band. Lifted the repeat-fits-its-cell check and
      the swept bound into free functions: 39 -> 9, worst in the file now 17.
      `ctape_prim_dist` stays at 56 — a primitive dispatch inside the kernel
      dialect, where restructuring risks all four backends. Flagged, not touched.
