# Tasks: scale-an-item-per-axis

- [x] 1.1 Establish that the engine can carry this at all, rather than taking #320's "the deformer is where it belongs" escape hatch: `cscale_nu_point` / `cscale_nu_dist` have been in `kernel/xform.h` since before the issue, `cfi_scale_nonuniform` classifies them, and one kernel test was the only caller in the tree
- [x] 1.2 Decide the representation: a `Node` field applied INNERMOST, not a widening of `math::Transform` — a Transform is a similarity and its algebra is closed only while it stays one, so widening it would make every composition a general matrix and take the exactness bookkeeping with it
- [x] 1.3 Compose it in `tape_build`: `S^-1` into the inverse matrix, `min(s)` into the scale slot — no new opcode and no wider tape record, because `[inv affine 12][scale][rounding]` already has the shape
- [x] 1.4 The copies and the payloads that travel with an item: mirror, radial, the feathered replace's sampled box, and the gate — each composes the same scale or is a differently-shaped copy of the same item
- [x] 1.5 Bounds and picking: `item_geometry_bound` and `node_shape_bounds`, plus the rounding and volume-band factors, which must use the SAME factor the field does
- [x] 1.6 Format: scene and `.clayspace` minor 14, three floats appended last in the node record and gated; `SetTransformCmd` carries them too, since it carries the whole transform
- [x] 1.7 C ABI: `clay_item_set_scale_nonuniform`, `clay_layer_set_transform_nonuniform`, `clay_layer_node_transform_nonuniform`, `clay_mesh_transform_nonuniform`; and `clay_layer_node_transform` REFUSES a node it cannot express
- [x] 1.8 pyclay: `scale=` takes one number or three everywhere it is accepted, and `set_transform` seeds the per-axis half from the node so a partial update leaves a squash alone
- [x] 1.9 Tests: 8 engine cases, 6 C ABI cases, 5 pyclay cases — each verified to FAIL against a targeted revert that still compiles (see 3.1)
- [x] 1.10 Docs: `docs/01-sdf-math-foundations.md` on the operator and its cost, `docs/05-claycore-library.md` and `docs/07-brushes-and-features.md` on the surface, `docs/RELEASE.md` on 0.54.0
- [x] 0.1 SEQUENCING: takes scene/clayspace minor 14 and ABI 0.54.0, so it serialises against any other change that takes a format minor

## Deliberately not in this change

- [ ] 2.1 **A layer's per-axis scale.** `clay_document_set_layer_transform` keeps
      its single factor. This is not more of the same work: `layer.xform *
      node.xform` is consumed as a rigid FRAME by `brush::move` and
      `brush::lattice_gizmo`, which place a manipulator and a cage in it, and a
      frame with a per-axis scale is not a `math::Transform` at all. What those
      two should do with one is a question #320 does not settle and the item arm
      does not need.
- [ ] 2.2 **A per-axis scale in `clay_item_desc`.** The flat descriptor is
      zero-filled by contract, so a zeroed scale would have to be read as
      (1, 1, 1) rather than as what it says. The builder path covers it and the
      header says so.

## Found while doing it

- [x] 3.1 **The failure check needs a revert that COMPILES.** The first attempt
      neutered `item_scaled_inverse` and left `item` unused, which `-Werror`
      rejected — so nothing was rebuilt and all twelve cases "passed" against
      the still-fixed binary. The same shape as the one #323 recorded a week
      ago. Three targeted reverts were needed in the end, because one revert
      cannot exercise the format gate and the composition at once: dropping the
      serialization ENTIRELY makes the downgrade case pass vacuously, and only
      writing the field UNGATED actually breaks it.
- [x] 3.2 **The exactness cost is not the one it looks like.** The first draft
      of the header said `clay_layer_safe_step_scale` reports the loss. It does
      not and cannot: `cscale_nu_dist` can only SHORTEN a distance, so the
      Lipschitz bound is unchanged at 1 and the step scale does not move.
      `clay_tape_info`'s `out_is_exact` is the only thing that reports it, and
      the corrected wording says so — a squash costs no speed, it costs the
      guarantee that the value IS the distance.
- [x] 3.3 **The normals threshold is measured, not guessed.** Over the 32210
      vertices of a meshed sphere squashed 3x, the worst agreement with the
      ellipsoid's gradient is 0.999999 through the inverse transpose and
      0.865830 if the normals are merely rotated. The test asserts 0.999, which
      separates the two by a wide margin in both directions; the 0.9 it was
      first written with would have passed for the wrong implementation at most
      vertices.
- [x] 3.4 **A blanket rename in `pyclay_module.cpp` caught three innocents.**
      Widening `float scale` to `nb::handle scale` hit `raycast_mesh`,
      `add_mesh_layer` and one flatten overload, none of which is a placement.
      Two were reverted and one kept; the compiler caught all three, which is
      the only reason a textual edit was safe here at all.
