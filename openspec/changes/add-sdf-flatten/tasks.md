# Tasks: add-sdf-flatten

- [x] 1.1 `field::flatten`: blend a sampled volume's stored samples toward a plane
- [x] 1.2 Two-sided, matching `sculpt_flatten`: remove above, fill hollows below
- [x] 1.3 ~~A bounded step per pass~~ — superseded: sampling makes the blend closed-form
- [x] 1.4 Declare the Lipschitz the result actually satisfies
- [x] 1.5 A region with a falloff, so it is a brush and not a global filter
- [x] 1.6 Python bindings, and a C ABI entry point beside `clay_item_volume_relax`
- [x] 1.7 Tests: a bump becomes a facet, a dent fills, a surface already flat does
      not move, the region is respected with no rim, the declared bound holds,
      a ray still lands, and more passes travel further
- [x] 1.8 Docs, example, full verification

Two things the proposal got wrong, found by rendering it:

- [x] 1.9 The proposal had flatten rewrite a volume's stored samples, as relax
      does, with a bounded step to keep the varying-weight term in hand. That
      mechanism cannot work: a band tracks the surface only while the surface
      stays inside it, and flatten moves it many band widths. Measured — eight
      strokes moved a surface 0.20 with a band of 0.05, and the isosurface came
      apart in fragments. Flatten now SAMPLES, so the band brackets the
      flattened surface, and the blend is closed-form: the step, the iterations
      and the band narrowing all went away with it.
- [x] 1.10 The region was specified as optional, "a filter rather than a brush".
      There is no such filter. Where flatten's weight is one the result IS the
      plane, so a region-less flatten at full strength replaces the shape with a
      half-space — the ball came back as a box, in a picture. A region is now
      required and its absence refused.
- [x] 1.11 The per-sample clamp the first implementation used broke the convex
      combination that keeps the blend 1-Lipschitz: two adjacent samples could
      saturate in opposite directions. Measured at 5.52 against a declared 3.46
      before the cause was understood. The Lipschitz is now measured from the
      samples produced rather than bounded in advance.
