# Tasks: add-flatten-modes

- [x] 1.1 `FlattenMode` on `FlattenSettings`, defaulting to today's two-sided
- [x] 1.2 The clamp, in the one place the blend happens
- [x] 1.3 The declared Lipschitz must still hold — a clamped blend is not steeper
      than the unclamped one, but measure it rather than assume it
- [x] 1.4 Python and C ABI, the latter by extending the versioned descriptor
- [x] 1.5 Tests: cut-only leaves a hollow alone and still cuts a bump; fill-only
      the reverse; two-sided is unchanged; every mode lands the cut side on the
      plane; the region is still required
- [x] 1.6 Docs, example, full verification

Found while building:

- [x] 1.7 The C ABI mode read landed in `clay_item_volume_relax` first: relax and
      flatten share the two lines `settings.region_radius = ...; settings.falloff
      = ...`, so a textual replace hit the wrong one. Scoped to the flatten
      entry point's body instead. `kFlattenParamsOriginal` stays pinned to
      `falloff`, which is what keeps a pre-mode descriptor valid — tested.
- [x] 1.8 The first render read as ambiguous: cut and fill BOTH showed a crater,
      and the caption claimed only one did. Measuring the flank profile rather
      than a single probe showed the picture was right and the caption was not —
      cut planes the proud flank and keeps the hollow, while fill closes the
      hollow and leaves the proud material standing as a rim. The example now
      prints the profile, which says more than either render.
