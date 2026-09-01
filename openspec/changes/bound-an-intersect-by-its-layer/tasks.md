# Tasks

## 1. The split

- [x] 1.1 `scene::Nonlocality` — None / BoundedByLayer / Unbounded — as the ONE
      place that says why an item's influence is not its own geometry, with the
      second-cause ordering explicit (an intersect that also repeats infinitely
      is unbounded).
- [x] 1.2 `layer_influence_extent`: the union of a layer's visible item
      geometry, infinite the moment one of them has none.
- [x] 1.3 `item_influence_bound` returns the layer's extent for an intersect and
      infinite for the rest, with an optional precomputed extent so a caller
      that already holds one does not recompute it.
- [x] 1.4 The same split for a GROUP whose own op is an intersect, and for the
      ancestor walk in `node_reach_bound`.
- [x] 1.5 `item_influence_is_local` UNCHANGED — the cull gate keeps refusing
      every non-local op — and `item_own_influence_bound` unchanged, with both
      reasons written down.

## 2. Evidence

- [x] 2.1 The measurement #326 asked for: rank the item's own box against the
      layer's extent on #319's own fixture, and require the tighter one to leak.
- [x] 2.2 The morph half: hold both morphs to the infinite answer, and REPORT
      rather than assert the leak — it is 4 points in 200,000 on arm64 and 0 on
      x86_64, so asserting it would gate a mechanical claim on float rounding.
- [x] 2.3 Raise the property test's non-local cases to a sample count that finds
      a one-in-ten-thousand violation every run, and say why the local corpus
      does not need it.
- [x] 2.4 Update every test that asserted the old contract to the new split,
      each keeping a case that still asserts infinite for what genuinely is.

## 3. Docs

- [x] 3.1 `docs/05-claycore-library.md`: the split, the mechanism behind it, and
      the measurement.
