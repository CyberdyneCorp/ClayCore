# Tasks: add-pose-line-regions

- [x] 1.1 Widen the deformer record to 12 floats and the extension array to 6
- [x] 1.2 `cpose_line_point` in `deform.h`; rotation about the axis through the anchor
- [x] 1.3 `cfi_pose_line` exactness helper
- [x] 1.4 `cdeform_pose_line` opcode, dispatch, `ext_count`
- [x] 1.5 `Deformer::pose_line` with a degenerate-segment guard
- [x] 1.6 Bounds: hull of original and rotated corners, dilated by the arc sagitta
- [x] 1.7 Both bindings
- [x] 1.8 Tests: tape-vs-kernel, anchor fixed and tip turned, projection not distance, easing shapes the taper, bound containment at large angles, old documents still load, round trip, C-vs-scene
- [x] 1.9 Parity scene; docs; ABI 0.11.0; full verification
