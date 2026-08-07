# Tasks: add-sdf-flatten

- [ ] 1.1 `field::flatten`: blend a sampled volume's stored samples toward a plane
- [ ] 1.2 Two-sided, matching `sculpt_flatten`: remove above, fill hollows below
- [ ] 1.3 A bounded step per pass, with iterations; widen a falloff too narrow for it
- [ ] 1.4 Declare the Lipschitz the result actually satisfies
- [ ] 1.5 A region with a falloff, so it is a brush and not a global filter
- [ ] 1.6 Python bindings, and a C ABI entry point beside `clay_item_volume_relax`
- [ ] 1.7 Tests: a bump becomes a facet, a dent fills, a surface already flat does
      not move, the region is respected with no rim, the declared bound holds,
      a ray still lands, and more passes travel further
- [ ] 1.8 Docs, example, full verification
