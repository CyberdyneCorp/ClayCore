# Tasks

- [x] 1.1 Lift a group's blend support out of `node_influence_bound` into a named function, so the group path and the ancestor walk share one definition
- [x] 1.2 Add the ancestor-path bound beside `node_influence_bound`: the node's own bound, dilated by each enclosing group's support, infinite as soon as an ancestor's combine is non-local
- [x] 1.3 `node_command_bound` uses it instead of the root ancestor's bound; the union over layers sharing the content is unchanged
- [x] 1.4 Property test: band-clamped values outside the ancestor-path bound are bit-identical across an edit to the child, on singly and doubly nested groups
- [x] 1.5 Test: a group holding one small child and one far large one reports a bound for the child that excludes the sibling and is strictly smaller than the group's
- [x] 1.6 Test: a child under an intersect group still reports unbounded
- [x] 1.7 The undo/redo bound scenarios in `c-abi` updated to the new rule, including the two new ones
- [x] 1.8 Measured. The bound IS tighter — for a grouped 1000-item document the command influence bound goes from 100% of the layer's extent to 49% (2.639 -> 1.300), i.e. ~8x smaller by volume — and the end-to-end effect is **nil**: every device case moved within +-2%, and an A/B on the Mac moved the grouped after-drag row 2.589 -> 2.458 ms (1.05x). See 1.11 for why
- [x] 1.9 The two DRAG rows did not move (13.74 -> 13.59, 14.28 -> 14.61), as expected
- [x] 1.11 **WAS blocked on the cull pad, now unblocked and the win is real.** With `hold-the-cull-pad-still` landed, A/B against the same tree carrying ONLY the pad fix (so the two changes are told apart rather than inferred), grouped document, host loop that re-evaluates its visible set, 512 bricks:

  | grouped, 1000 items | bricks resumed/frame | ms/frame |
  |---|---:|---:|
  | pad fix only, old root-ancestor bound | 0 | 44.17 |
  | pad fix + this change | **285.2** | **32.85** |

  **1.34x**, and 1.37x at 100 items. The flat control is unchanged (23.9 -> 23.6 ms, 377.5 resumed either way), as it must be: a flat document has no groups. Still zero on a host that only refills the dirty set — that half of the finding stands. ORIGINAL NOTE, kept because the reasoning was right and the conclusion was premature: A brick's seed is dropped when the edit's region intersects that brick DILATED BY `band + pad`, and `pad` is the chain pad of the whole document. At 1000 items with blends it is wide enough that even the tightened box reaches every seed in the volumes measured: a store of 64 bricks over a 1.6-unit volume goes to 0 entries on ONE drag frame, flat or grouped, before and after. Until `narrow-the-chain-pad` lands there is no workload where a tighter command bound can show. Re-measure the pair after it
- [x] 1.12 **FIXED.** The device pair conflated two things: `sdf_stamp_after_group_drag_bricks` adds its stamp INTO the group, and `tail_append` requires `parent == kNoNode` — so the grouped row also loses the append fast path and takes a structural invalidation. Measured on the Mac at 1000 items: stamp into the group 7.6 ms, the same stamp at the layer root in the same grouped document 2.5 ms, flat 1.5 ms. So of the reported 3.9x, roughly 3x is the append path and 1.6x is the group's compile cost — and none of it is the invalidation bound. The stamp now goes to the layer root in both shapes and the pair reads **1.08 vs 1.15 ms — 1.06x** on the reference iPad. The append-path half has its own case (`sdf_stroke_in_group_bricks`) and its own change (`append-into-a-group`), where it measures **89.9x**
- [x] 1.10 `docs/` — the transform numbers, the corrected attribution, and the append-path pair
