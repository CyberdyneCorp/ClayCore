## 1. Reproduce

- [x] 1.1 Seed 5128, rich, `RV_RAWBOUND` on main (8b7a5713): 10 samples in 2 directions
- [x] 1.2 Isolate: the node alone shows nothing; with either smooth sibling after it, the violation returns
- [x] 1.3 Generalise: two spheres, the first moved, the second smooth at k = 0.3: 6,359 band samples outside box + band, worst 0.0438, 25 stale bricks from `mark_dirty_nodes`

## 2. Fix

- [x] 2.1 `node_reach_bound` dilates at each level by the later siblings' `cull_pad_terms`, resolved at full support (`CullPadTerms::support_total`)
- [x] 2.2 Resolve at full support, not at the envelope (0.0123 left at the envelope)
- [x] 2.3 Leave the #639 head reach alone (raw-exact outside its ball); the clamp into the node's bound is what needed the term
- [x] 2.4 `ChainDragMemo` in `LayerExtent`, threaded through the drag frontier and shared across an undo replay, cleared after any command but a deformer or colour edit

## 3. Prove

- [x] 3.1 `test_node_reach_bound.cpp`: a smooth sibling after a node carries its edit past its box, at the root and in a group; only a later smooth combine widens; a later sibling of the node's group drags it; the replay memo is cleared (fails with the clear removed); the memo equals the walk
- [x] 3.2 `test_c_undo_bound_grab_support.cpp`: a ball past the node's band on a smooth sibling's surface, undone and redone, refills what a rebuild does (4 stale bricks per direction on main)
- [x] 3.3 New cases fail on main, pass here; full unit suite 10 / 10 shards
- [x] 3.4 Oracle, 400 plain + 400 rich: raw-bound violations 0 / 10 -> 0 / 0, stale trials unchanged, refills +2.5% / +2.8%
- [x] 3.5 Cost: drag frontier 1,428 warps over 10,000 items 0.39 -> 0.45 ms; undo of a 2,134-warp Move 0.85 -> 1.04 ms

## 4. Document

- [x] 4.1 `clay.h` note on `clay_layer_node_influence_bound`, `docs/05`, the scene-model requirement
