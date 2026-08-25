# Tasks: pad a cull for the chain, not just for one blend

## 1. Establish that it is a defect

- [x] 1.1 Band-only dilation on a blended document: in-band disagreements at
      every chain length above ~5, worst 0.009 at 25 items — half a cell.
- [x] 1.2 Hard union, same shapes, same dilation: zero at every length. So the
      per-item bound test itself is sound and the blend is what breaks it.
- [x] 1.3 A dilation sweep to tell a dropped contributor from float
      re-ordering: the error is CONSTANT from band+0.00 to band+0.11 and zero
      from band+0.12, with the instruction count climbing smoothly throughout.
      A threshold, not a decay.
- [x] 1.4 The mechanism, measured rather than argued: the chain drags the field
      0.26 below the base shape's own distance at k=0.06, against 0.04 for the
      same shapes hard-unioned.
- [x] 1.5 That the brick cache is exposed: it clamps values to the band, and
      these disagreements are INSIDE the band, so clamping does not hide them.

## 2. The fix

- [x] 2.1 `scene::cull_pad` — the feather term and the chain term, in one walk.
- [x] 2.2 Applied by the COMPILER on top of the caller's region, exactly where
      the feather pad already was. Callers unchanged.
- [x] 2.3 One walk rather than two: each walked every node in the layer and the
      compiler asks per uncached compile. Measured 20-30% on the per-brick cull
      benchmarks at ten thousand items before merging them.
- [x] 2.4 `CullIndex::feather_pad` -> `cull_pad`, since it stopped being only
      the feather. The old name would have been a lie the compiler relied on.

## 3. Tests

- [x] 3.1 A 200-item blended chain through `check_document`, the same helper
      the corpus tests use. That helper ALREADY asserted this exact contract —
      what it lacked was length, which is why a corpus containing blends passed.
- [x] 3.2 VERIFIED to fail without the pad: 9 assertions, by zeroing the blend
      term and rebuilding.
- [x] 3.3 The cached-pad assertion now checks the pad equals what the compiler
      would recompute, rather than asserting zero.
- [x] 3.4 Full unit suite: 1417 cases, no failures.

## 4. Cost

- [x] 4.1 20-35% on `DeepDocCull*`, whose document blends at k=0.03. This is
      the fix, not an accident: a wider region keeps items that matter.
- [x] 4.2 `check_bench.py` passes; no ratio gate moves.

## 5. Documentation

- [x] 5.1 `tape.h` states the compiler's own pad, both reasons for it, and that
      the chain term is a well-tested bound rather than a proof.
- [x] 5.2 `BrickCache::cull_region` says why it dilates by the band alone.

## 6. Not in this change

- [ ] 6.1 A provable bound for an arbitrary chain. The drag grows with length;
      no fixed dilation covers every document.
- [ ] 6.2 Per-brick culling of the BAKE (#277), which this unblocks.
