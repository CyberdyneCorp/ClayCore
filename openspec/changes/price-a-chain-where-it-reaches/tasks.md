## 1. Price each link on its own

- [x] 1.1 `chain_links` prices every link against the identity and records
      whether it composes by multiplying (a point warp) or by adding (a
      distance offset)
- [x] 1.2 `link_support` gives the ball each finite-support kind stops acting
      outside, and how far it can move a point
- [x] 1.3 A degenerate radius is treated as UNBOUNDED, not as an empty ball —
      the other way round is the unsafe direction

## 2. Group and fold

- [x] 2.1 `group_by_reach` groups the finite links that can reach one another,
      the gap threshold being `r_i + r_k + total chain travel`
- [x] 2.2 `deformer_lipschitz` folds once per group with the other groups
      absent, and takes the worst; an unbounded link is in every group

## 3. Proof

- [x] 3.1 Eight disjoint grabs cost what one costs
- [x] 3.2 Grabs that CAN reach each other still compound, every extra one
      costing
- [x] 3.3 The threshold is the reach and not the radii alone: two balls that do
      not overlap still compound when the chain can carry a point between them
- [x] 3.4 A disjoint chain is still safe to march by
- [x] 3.5 An unbounded link is charged against every group, and the answer is
      exactly the product — proven with a twist WEAKER than the grabs, so no
      single term can reach it by accident
- [x] 3.6 `pose_line` is not treated as finite support, proven with anchors
      placed symmetrically so the two poses have equal factors
- [x] 3.7 A distance offset still composes by adding, wherever it is grouped
- [x] 3.8 Each claim fails when its own half of the change is reverted, and the
      two over-relaxations (a finite `pose_line`, unbounded links dropped from
      the per-group fold) each fail a test
