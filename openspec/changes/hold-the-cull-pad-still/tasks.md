# Tasks

- [x] 1.1 `chain_pad_envelope` quantises its growth above the base, rounded UP, to a stated step (`kEnvelopeQuantum` = 0.25 in k-multiples); the step is sized so one 24-dab stroke crosses at most one boundary
- [x] 1.2 Test: appending a node between steps leaves `cull_pad` bit-identical (exact `==`, since the seed gate is exact and an approximate check would pass for a pad that still invalidated everything)
- [x] 1.3 Test: the quantised value is never smaller than the fit, at every node count across the band, on all three measured profiles
- [x] 1.4 Test: a culled brick still agrees with the full tape on band-clamped values across the band — bounded drift and reported, the convention `test_cull_index.cpp` already uses. Worst 1.5e-6, falling with document size
- [x] 1.5 Test: the envelope is monotone and steps 7 times between 1 and 2000 nodes rather than ~2000; and a 24-dab stroke crosses at most one step from every start between 60 and 1000
- [x] 1.6 `SceneBuilder`: a smooth-blended stamp and dab, beside the hard-blended ones rather than replacing them
- [x] 1.7 `sdf_stroke_smooth_bricks`, on an axis that straddles the band (10/300/800/2000)
- [x] 1.8 Measured on the reference iPad: **30.5x at 300 stamps, 27.7x at 800**, flat across the axis afterwards (0.144 / 0.145 / 0.147 / 0.161 ms per dab, 38.5 bricks resumed at every point). `sdf_stamp_bricks` and `sdf_stroke_bricks` unmoved, as expected — they are hard-blended and their pad is exactly 0, checked directly
- [x] 1.9 `docs/09-brush-latency-and-coverage.md` — the band, what it cost, the fix, and that the SDF fixtures were hard-blended
- [x] 1.10 Budget re-derived from the fixed run: **158.85 ms -> 5.79**, a 27x tightening. A ceiling kept from the hump would have been the `a budget can be too loose to fail` failure exactly

## Noted, not this change

- [ ] 2.1 The gate now reports `sdf_stamp_bricks` at 0.114 ms against a 0.708 ms budget — 6.2x loose. NOT caused by this change: that fixture is hard-blended and its `cull_pad` is exactly 0, checked directly. It is the committed baseline standing at ABI 0.56.0 while main is 0.60.0. Re-derive it in the next full gate run, with `add-device-transform-cases` task 1.12
