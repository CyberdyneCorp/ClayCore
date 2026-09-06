## 1. The bound

- [x] 1.1 The travel budget between two links is the travel between them
- [x] 1.2 Each link is priced against its neighbourhood, not its component

## 2. Gates

- [x] 2.1 A walked stroke is not charged as one clique — 16 grabs at 0.647
      against 2 at 0.748, where the old grouping gave 0.098
- [x] 2.2 The relaxed chain is still a BOUND — `check_conservative_steps` on it
- [x] 2.3 The existing threshold case restated against the real reach: a pair at
      1.2 with radius 0.3 and pull 0.5 genuinely cannot meet, because a point
      leaving the first ball reaches 0.8 and the second begins at 0.9. The case
      asserted the whole-chain budget, which was the implementation and not a
      truth
- [x] 2.4 Grabs piled on one spot still compound; an unbounded link is still
      charged against every group (both unchanged and still passing)

## 3. Not attempted

- [ ] 3.1 Bounding the warp COUNT. At 50 grabs, 1,673 warps are evaluated per
      sample and raycast/mesh stay near 1000/1800 ms. Per-region culling cannot
      help (a ray crosses many balls); regional consolidation reclaims it all
      but costs 19.4 s, or 122 s per-patch. That is
      `price-the-warps-a-layer-carries`' parked design question and #452 stays
      open on it.
