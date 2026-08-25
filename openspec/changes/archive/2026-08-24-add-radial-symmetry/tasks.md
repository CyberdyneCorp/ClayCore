# Tasks

## 1. The model

- [x] 1.1 `Layer` gains `radial_count` (uint16, 0/1 = off), `radial_axis`
      (uint8, 0/1/2, default 1 = Y) and `radial_k` (float seam blend), beside
      `mirror_axes` and `mirror_k` in `scene/document.h`
- [x] 1.2 `math::rotation_matrix(int axis, float radians)` beside
      `reflection_matrix`, with the same "affine matrix, not a Transform"
      framing so the two read alike
- [x] 1.3 `SetLayerRadialCmd` beside `SetLayerMirrorCmd`, in the command variant,
      with the same lock check and the same inverse-is-the-previous-value shape

## 2. Evaluation

- [x] 2.1 `tape_build.cpp emit_item` emits `count - 1` rotated instances for a
      participating item, combined with `Op::Add` under the seam blend, and marks
      the tape smooth-blended when the seam is positive
- [x] 2.2 The feathered-replace exclusion that applies to the mirror applies here
      for the same reason, and the comment says so once rather than twice
- [x] 2.3 `bounds.cpp` covers every emitted copy, dilated by the seam support

## 3. The surfaces

- [x] 3.1 Serialization: three fields after `mirror_k`, gated on the scene minor;
      `kSceneMinor` and `kClaySpaceMinor` both move
- [x] 3.2 `clay_set_layer_radial` in the C ABI, rejecting an out-of-range axis and
      a negative blend rather than clamping
- [x] 3.3 `Layer.radial(axis="y", count=..., blend=...)` in pyclay, matching
      `Layer.mirror`'s shape
- [x] 3.4 `check_binding_parity.py` passes with no new exemption

## 4. Tests

- [x] 4.1 The field is invariant under rotation by `2π/count` — sampled, not
      asserted structurally
- [x] 4.2 Clearing the count restores the field pointwise
- [x] 4.3 An item with participation cleared appears once; every other appears
      `count` times
- [x] 4.4 A stroke on a radial layer repeats without the caller touching its
      resolved nodes
- [x] 4.5 Undo of a radial change restores the previous count, axis and blend
- [x] 4.6 A document round-trips the mode; a document written at the previous
      minor loads with it off
- [x] 4.7 Radial and mirror together emit the additive set, not the product

## 5. The record

- [x] 5.1 `docs/07-brushes-and-features.md` §1's symmetry section covers both
      modes and says which is a mode and which is a modifier
- [x] 5.2 README's symmetry line and the verb table in §11 name it
- [x] 5.3 `openspec/ROADMAP.md`: the gap row and the P2 line move from unscoped
      to scoped, the way the surface-groups and history rows already read
