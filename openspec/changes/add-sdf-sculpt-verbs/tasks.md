# Tasks: add-sdf-sculpt-verbs

- [ ] 1.1 Design note: how a region-weighted field-shaping instruction sits in the tape after the item chain, and whether later items see its result
- [ ] 1.2 `cop_region_weight` shared by all four verbs — one falloff, so they cannot drift apart
- [ ] 1.3 `sculpt_inflate` / `sculpt_pinch`: weighted offset and weighted pull toward the region axis (no neighbour sampling)
- [ ] 1.4 `sculpt_flatten`: weighted pull toward a plane, refusing a zero-length normal as the voxel verb does
- [ ] 1.5 `sculpt_smooth`: sample pattern, cost multiplier, and the Lipschitz factor it declares
- [ ] 1.6 Commands + undo inverses; `.clayspace` round trip, bit-identical
- [ ] 1.7 Both bindings, with the C ABI additive
- [ ] 1.8 Tests: each verb's effect and its falloff to zero at the region edge; a verb over empty space is a no-op; smooth reduces surface curvature without moving the silhouette beyond tolerance; step scale reported honestly; parity across every registered backend
- [ ] 1.9 Example: the arm/torso seam from 34_organic_character, smoothed
