# Tasks: do not re-derive far bounds a shrink cannot have changed

## 1. The guard

- [x] 1.1 `shrink_band` computes the narrowed band first and returns when it
      equals the one already held.
- [x] 1.2 The comment states WHY it is exact rather than that it is:
      `build_far_bounds()` depends on the stored-brick set, the grid and the
      band; an in-place rewrite changes neither of the first two.
- [x] 1.3 And that it is the steady state rather than an edge case — a bake
      starts at three cells, the floor is two, so only the first dab of a
      stroke narrows anything.

## 2. Tests

- [x] 2.1 A shrink that DOES narrow still changes the serialized volume, so the
      case below cannot pass by `shrink_band` having become a no-op outright.
- [x] 2.2 A shrink at the floor leaves `serialize()` unchanged byte for byte,
      twice over — once with a modest `by` and once with one ten times larger.
- [x] 2.3 And `eval()` at 500 points across the region is unchanged. The bytes
      are a proxy; what a far bound is FOR is what it reports in the empty
      majority of the region, so the test asks it there.
- [x] 2.4 Full unit suite: 1417 cases, no failures. `BM_VolumeBake*` and
      `BM_Consolidate*` unmoved.

## 3. Measured

- [x] 3.1 A stroke of 24 dabs, each timed, rather than one dab on a fresh
      volume. The distinction is the whole point: a fresh volume is always the
      FIRST dab, which legitimately narrows, so a per-dab-on-fresh-volume
      benchmark shows nothing at all. This was measured the wrong way first and
      read as no change.
- [x] 3.2 cell 0.01: steady p50 2.321 -> 1.767 ms, 1.31x. First dab unchanged.
- [x] 3.3 cell 0.02: no measurable change; the chamfer was already small there
      next to the other terms.

## 4. Not in this change

- [ ] 4.1 The per-pass volume copy (0.162 ms, ~9% of a dab now). Needs a
      region-scoped snapshot rather than a guard. #278 stays open for it.
- [ ] 4.2 Caching per-brick chamfer steps, which #278 proposed. It is the right
      fix for a shrink that DOES narrow and unnecessary for one that does not —
      and adding an array to every volume to speed a once-per-stroke event is
      the wrong trade.
