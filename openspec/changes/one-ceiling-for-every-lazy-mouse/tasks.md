## 1. The findings

- [x] 1.1 Lazy mouse is applied ONCE in stroke resolution, so stamp-based
      brushes all have it; gestures never pass through resolution
- [x] 1.2 `clay_voxel_grab_update` has `clay_sdf_move_update`'s exact shape --
      a total displacement from an anchor -- and had no lag
- [x] 1.3 `steady_path` clamped to 0.95 while `clay_sdf_move_begin` refused only
      at >= 1.0, so 0.99 behaved differently on the two paths

## 2. Ruled OUT, with reasons, so no inert field is added for symmetry

- [x] 2.1 `clay_sdf_smooth_update` takes no position -- nothing to lag
- [x] 2.2 `clay_multires_sculptor_begin_stroke` goes through stamps, so
      resolution already covers it
- [x] 2.3 `clay_layer_placement_begin` is a gizmo, not a brush

## 3. The change

- [x] 3.1 `clay_voxel_grab_set_steady`, on the TRANSACTION rather than in
      `clay_brush_params`, which is shared with stamp-based calls where a lag
      is inert
- [x] 3.2 One shared `kSteadyCeiling`, hoisted so both gesture paths use the
      same constant rather than two literals that can drift
- [x] 3.3 The gesture paths refuse above it; the stroke path keeps clamping,
      and changing that is left as a separate decision

## 4. Tests

- [x] 4.1 A lagging grab lands somewhere else after one update, and CONVERGES
      when held -- a lag that never arrived would be a different defect
- [x] 4.2 The fixture asserts the drag moved something, or the comparisons are
      between identical untouched grids
- [x] 4.3 0.95 accepted, 0.96 and 1.0 and -0.1 refused, null refused
- [x] 4.4 Full suite 11/11, c-abi OK, binding parity OK

## 5. Still open

- [x] 5.1 Device gate before any tag carries this -- RAN: 7/7 sessions, 75 cases, 0 failures on iPad15,5 / iOS 26.5.2 at d391f817, tagged v0.113.0
- [ ] 5.2 Whether `steady_path` should refuse rather than clamp. More consistent
      with the house style, but it changes behaviour for existing preset callers
