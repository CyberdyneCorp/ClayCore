# Tasks

- [x] 1.1 `SceneBuilder.dragPosition(_:)` — a walking placement path, short enough to stay inside the working volume at every axis size and stepped well under the dab radius so consecutive frames overlap; wraps at 32 so a long pass cannot walk the node into empty space
- [x] 1.2 `SceneBuilder.sdfDocumentGrouped(stamps:)` and `addDragNode` — the same document with every item inside one group, differing from the flat one in that and nothing else
- [x] 1.3 `sdf_node_transform_bricks`: drag a node at the layer root, dirty by node, refill; no reset; bricks per frame reported
- [x] 1.4 `sdf_group_transform_bricks`: the same drag with the node inside a group
- [x] 1.5 `sdf_layer_transform_bricks`: drag the layer's placement, dirty by layer, refill
- [x] 1.6 Guard in each case: a frame that refilled nothing fails, as `sdf_stamp_bricks` guards
- [x] 1.7 `sdf_stamp_after_drag_bricks` / `sdf_stamp_after_group_drag_bricks` — the pair that can see what the drag rows cannot, with the drag frame in the reset so it is paid for and not timed
- [x] 1.8 `VERB_PATTERNS` matches the placement entry points; coverage-table entries for all five cases and an exemption for the per-axis form
- [x] 1.9 Run on the reference iPad; `check_device_coverage.py` reports all 5 GATED rather than REPORTED ONLY
- [x] 1.10 Budgets written by hand into `tests/device/baseline.json` from that run — appended, never re-sorted and never by `--update`, which rewrites every case from one run
- [x] 1.11 `docs/09-brush-latency-and-coverage.md` — the inventory rows, the frame share, and the corrected attribution
- [x] 1.13 **Correction.** The after-drag pair added its stamp INTO the group in the grouped case, which also cost it the append fast path — so its 3.9x was mostly `tail_append` refusing a non-root parent, not the invalidation. The stamp now goes to the layer root in both shapes and the pair reads 1.08 vs 1.15 ms. Budgets re-derived
- [x] 1.14 `sdf_stroke_in_group_bricks` added, since the correction left the append-path finding with no case
- [x] 1.15 **Verified, and corrected again.** The first version built the whole document inside a group, varying the base document's shape as well as the dab's parent. An independent C-ABI harness separated them: with the base held identical, dabs at the root are flat at ~0.038 ms/dab over 10..3000 items and dabs into a group grow 3.0x -> 176x. The device case now adds an empty group at the tail of the root list and puts only the DABS in it, so the pair varies one thing; the ratio was unchanged by the correction (89.9x -> **90.4x** at 1000 items). Budget re-derived
- [x] 1.16 The control turned up a separate, unexplained anomaly: base document grouped but dabs at the ROOT measures 1.1x at 10/30/1000/3000 and **8.4x at 100, 19.7x at 300**. Reproduces exactly; not the append path; unmeasured by anything. Recorded in `append-into-a-group` and in docs/09 as a finding, not folded into any claim
- [ ] 1.12 Fold the five cases into the next FULL gate run, so `baseline.json` stops holding five entries measured at ABI 0.60.0 beside 62 measured at 0.56.0
