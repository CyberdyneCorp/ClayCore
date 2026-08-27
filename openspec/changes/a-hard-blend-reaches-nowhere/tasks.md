# Tasks

- [x] 1.1 Reproduce #335 against v0.39.0 and main from one phase-timed harness
- [x] 1.2 Show the meshing is NOT the regression: identical cache, identical triangles, 1.24x faster
- [x] 1.3 Attribute it to #282's pad: zeroing the blend term restores v0.39.0's tape count exactly
- [x] 1.4 Model the pad's cost against k, and confirm the k=0.03 row is the "20-35%" that change recorded
- [x] 1.5 `scene::chain_drag_reach` answers 0 for a hard non-paint, non-extended combine
- [x] 1.6 Its ONLY callers are `blend_cull_pad` and `cull_pad`
- [x] 1.7 Paint keeps its drag, because its colour fades over max(support, k) whatever the profile
- [x] 1.8 An extended mode keeps its drag, because it ignores the profile by design
- [x] 1.9 The node's own bound keeps `max(support, k)`, and bounds.h says why it must
- [x] 1.10 Measure the first attempt's over-reach: 540 -> 10,105 disagreements for narrowing the bound too
- [x] 1.11 Confirm narrowing the pad alone is 540 -> 540 on that same document, exactly
- [x] 1.12 A regression test: the drag, the pad, the bound that must NOT move, and the field over a mixed chain
- [x] 1.13 Sweep the field case hard enough to catch what 24 bricks missed on x86-64 and arm64 did not
- [x] 1.14 Verify the test FAILS for BOTH mistakes — the pad not narrowed, and the bound narrowed
- [x] 1.15 `BM_DeepDocCullPlanned2000K06` and its ratio gate, so the pad is measured where it bites
