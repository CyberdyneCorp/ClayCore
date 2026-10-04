## 1. Reproduce

- [x] 1.1 The issue's stroke through the C ABI on main (ca5883b9): earlier stamp 4.72 wide against 0.72 for the last; `mark_dirty_nodes` 2,548 bricks against 48 for one stamp, relief and incise alike

## 2. Fix

- [x] 2.1 `edit_is_confined_to_own_bound` (`src/scene/bounds.cpp`): a relief or incise item, not a group
- [x] 2.2 `node_reach_bound` skips the downstream drag and the group support for such a node, and still walks the ancestors for visibility and non-local groups
- [x] 2.3 Leave the cull pad, `raise_blend_term` and the term for nodes before a relief item as they are

## 3. Prove

- [x] 3.1 `test_c_relief_stroke_reach.cpp`, the count: every stamp no wider than the last, inside the stroke's footprint, and the stroke's bricks within twice one stamp's (48 = 48) -- fails on main with 4.72 and 2,548
- [x] 3.2 `test_c_relief_stroke_reach.cpp`, the oracle: each stamp removed, and moved, with a cache refilled over its reported bounds bit-identical to one rebuilt from nothing, at the root, on a longer stroke, inside a smooth group followed by a smooth sibling, and on a mirrored layer, relief and incise; each edit is shown to change bricks and to leave them stale when nothing is marked
- [x] 3.3 Mutation: the reach shrunk by 0.1 for a confined node fails the oracle (20 edits stale, 1-6 bricks each)
- [x] 3.4 `test_node_reach_bound.cpp`: a relief or incise item's reach equals its own bound in a smooth group with a relief and a smooth sibling after it (fails on main by 3.2 per side); a relief group is still widened
- [x] 3.5 The #650 two-sphere tests, the memo tests and `test_c_undo_bound_grab_support.cpp` unchanged and passing; full unit suite 10 / 10

## 4. Document

- [x] 4.1 `clay.h` note on `clay_layer_node_influence_bound`, `bounds.h`, `docs/05`, the scene-model requirement
- ABI unchanged: 0.123.0. No entry point, descriptor or format change.
