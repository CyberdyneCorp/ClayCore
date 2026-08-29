# Tasks

- [x] 1.1 Establish by measurement what a mirrored grab does today: the copy under the ball does not move (0.00000 against -0.05945); the +x and -x drags differ by the whole pull (5,536 samples)
- [x] 1.2 Prototype "reflect the brush": select on the item's own bound against every image; confirm bit-identical +x/-x fields on an identity layer transform and ≤ 9e-8 on a placed one
- [x] 1.3 Settle the straddler: one warp per node carrying one grab per reaching image, ordered by value (centre and displacement); no dedupe of coincident images, with the continuity numbers
- [x] 1.4 `scene::item_own_influence_bound` as a pure factor-out of the geometry bound (`geometry_bound(item, layer, with_copies)`)
- [x] 1.5 `brush::drag_images` (additive: ball, one reflection per axis, one rotation per radial copy, in layer-local coordinates) and `MoveWarp{node, deformers, gesture}`; `moved_chain` replaces every leading grab of the gesture
- [x] 1.6 Repeat `emit_item`'s participation gate in the brush (opt-out and feathered replace see the ball alone); cross-reference both sites
- [x] 1.7 `clay_layer_move_surface`: one reach box per image; `touch_regions` / `touch_regions_from` over a span of boxes
- [x] 1.8 Regression tests, each verified to fail by reverting its guard: selection ({A, B} not the base; B's grab at local x -0.1 not 2.1), material under a copy moves, +x/-x bit-for-bit, straddler one grab per image and no stacking, value order, on-plane pinch, opt-out, x|y two images, radial, non-local, feathered replace, mid-drag reach change
- [x] 1.9 C ABI regressions on the 325-item fixture: same ridge set mirrored and unmirrored, node 1 absent; 12-segment gesture 244 warps either way; straddler counted once
- [x] 1.10 Frontier regressions: mirrored drag states ordinal 302, resumes as the unmirrored control, matches a fresh oracle; the reflected side is not served stale (was 997/8,192 samples)
- [x] 1.11 Benches `BM_MoveDragMirrored1000` (warped_ratio counter), `BM_MoveDragRefillMirrored` / `MirroredCold`; gates in check_bench.py; full gate run
- [x] 1.12 Docs: the Move section's "under symmetry" paragraph; clay.h; this change folder
- [x] 1.13 Compose with the prepared drag (add-sdf-sculpt-transaction): `PreparedMove` carries its images (`PreparedImage`: local centre, reach, the copy's map); `resolve_prepared_move` yields one grab per reaching image; `SdfMoveTransaction` previews and commits every image's grab; `clay_sdf_move_preview_grab_count` / indexed `clay_sdf_move_preview_grab`; parity tests under a mirror, two axes, a placed layer and a radial count; a live drag under a mirror equals the one-step commit
