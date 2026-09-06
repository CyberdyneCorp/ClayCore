# Tasks

## 1. The three calls

- [x] 1.1 `clay_layer_snap_to_ground`, `clay_layer_centre_bounds` and
      `clay_layer_zero_to_origin` in `bindings/c/clay.h` and
      `bindings/c/clay_c.cpp`. The bound (`layer_world_bounds`) and the command
      (`scene::SetLayerTransformCmd`) both already exist and neither moved —
      but "no change to `src/`" was REFUTED: pyclay applies commands itself
      rather than crossing the C ABI, so the write policy 1.2 asks for has to
      sit below both bindings. Four functions landed in
      `include/clay/scene/placement.h` / `src/scene/placement.cpp`; `scene` is
      the right module by `check_layering.py` (they need `math` and the command
      vocabulary and nothing else). See "What building it found" in
      `design.md`.
- [x] 1.2 One shared helper on the inside taking the layer and a world-space
      delta, so the three cannot drift on WHICH placement fields survive. It
      reads the placement per axis, adds the delta to `xform.position` alone,
      and applies ONE `SetLayerTransformCmd` carrying the rotation, the uniform
      factor and `scale_axes` through unchanged. The single-factor reader and
      setter are never used: the reader refuses a squashed layer and the setter
      clears the squash. REFINED while building: the C reader/setter pair is
      not bit-exact in EITHER half — the reader hands the rotation back through
      `atan2` and the setter rebuilds it through `sin`/`cos`, and the per-axis
      reader answers the PRODUCT of the two scales, so the setter would fold
      `xform.scale` into `scale_axes`. `scene::translated_layer_command` takes
      the `scene::Layer` and copies all three across untouched instead.
- [x] 1.3 `zero_to_origin` sets `xform.position` to zero and reads no bounds —
      it shares 1.2's write path but not its bound query, which is why it
      accepts an empty and a radial layer.
- [x] 1.4 The refusal set from `design.md`, in this order so a locked layer
      never costs a bounds walk: unknown id -> `CLAY_ERROR_NOT_FOUND`;
      ghosted or locked -> `CLAY_ERROR_INVALID_ARGUMENT`; non-finite
      `ground_y` -> `CLAY_ERROR_INVALID_ARGUMENT`; radial mode ->
      `CLAY_ERROR_INVALID_ARGUMENT` naming the mode; no material ->
      `CLAY_ERROR_INVALID_ARGUMENT`. A gesture already refuses every edit and
      needs no case here — assert it rather than write it. ONE ROW WAS
      MISSING and was added: an UNBOUNDED box (a plane, an infinite cylinder)
      reports faces of ±FLT_MAX rather than an infinity, so nothing raised and
      the layer would have been placed at 3.4e38 with a tape of NaNs. Both
      bounds-reading calls refuse it; `zero_to_origin` accepts it.
- [x] 1.5 Header documentation beside the three, carrying what they do NOT
      promise: the box is not the silhouette (a subtract item's box counts, a
      smooth blend can bulge past it, hidden items are excluded), idempotence
      holds to an ulp and not to the bit, no delta is returned and how to get
      it, an instance is placed and never severed, and the radial refusal with
      the widening that would lift it. Name the rejected designs the way the
      neighbouring entries do: the occupancy-weighted centroid, the
      pivot-centring reading, the out-parameter delta, and answering for a
      radial layer anyway.

## 2. Tests

- [x] 2.1 **The defect the change exists for**: a layer carrying three
      different scale factors takes all three calls, and afterwards its
      per-axis placement reads back the same three factors and the same
      rotation, bit for bit. Written to fail against the read-modify-write
      through `clay_document_set_layer_transform`, which clears them.
- [x] 2.2 Each rule against a hand-computed box: the snap puts `bounds_min.y`
      at `ground_y`; the centre puts the box centre at the origin in all three
      axes; zeroing leaves the box where the layer's own origin puts it.
- [x] 2.3 The same three on an SDF, a voxel and a mesh layer, each with a
      non-identity rotation and a per-axis scale, so `position` being outermost
      is held for every representation rather than for the one that was easy.
- [x] 2.4 One undo step each, and one invalidation: undo restores the previous
      placement exactly, redo restores the new one.
- [x] 2.5 The refusals, one case per row of the table in `design.md`, each
      asserting the document is unchanged afterwards — including the two rows
      where `zero_to_origin` is accepted and the other two refuse.
- [x] 2.6 A hidden layer is accepted (bounds read content, not visibility) and
      a hidden ITEM is excluded from the box, so hiding the lowest root moves
      where the next snap lands. The hidden-ITEM half is in
      `tests/unit/test_computed_placement.cpp` rather than in the C ABI file:
      the ABI has `clay_document_set_layer_visible` and NO entry point for a
      node's visibility, so that case cannot be built from the outside.
- [x] 2.7 An instance is placed and not severed: snap one of two layers sharing
      an edit list, then assert the other's placement and bounds are unchanged
      and `clay_document_layer_info` still reports both sharing.
- [x] 2.8 Idempotence to a tolerance: a second snap moves the layer by at most
      one ulp at the box's magnitude. Not a bit-equality assertion — see
      `design.md`. WHICH magnitude was wrong as written and is the change's one
      measured refutation: at y = 2048 snapped to y = 0.25, the second press
      moves 7.27e-06 against an ulp of 1.19e-07 at the RESULTING coordinate.
      The bound is one ulp at the magnitude the FIRST press read (2.44e-04),
      because a snap from far away lands the layer near the plane while the
      arithmetic ran far from it. The spec scenario and the test say so, and a
      third press is asserted to move no further than the second.

## 3. Python and the parity gate

- [x] 3.1 `Layer.snap_to_ground(ground_y)`, `Layer.centre_bounds()` and
      `Layer.zero_to_origin()` in `bindings/python/pyclay_module.cpp`, raising
      where the C ABI refuses. The names cross to C by the existing `Layer` ->
      `clay_layer_` prefix rule in `tools/check_binding_parity.py`, so no alias
      entry and no exemption is added — if either turns out to be needed, the
      names are wrong, not the table.
- [x] 3.2 `python3 tools/check_binding_parity.py --pyclay <build>/bindings/python
      --require-import` and READ THE LINE IT PRINTS: `imported <path>` is a real
      check, `parsed bindings/python/pyclay_module.cpp` is the fallback that
      compares the source against itself and cannot fail.
- [x] 3.3 No Swift work: `bindings/swift` is a link target
      (`ClayCoreLink/Empty.swift`) and exposes no per-call surface to follow.

## 4. Version and gates

- [x] 4.1 **ABI 0.85.0 -> 0.86.0**, moved in the PR that adds the entry points,
      not at release time. Done by the reconciling commit on this branch, which
      moved all three lines once for the PR's four entry points. NOT DONE HERE — the three entry points landed at
      0.85.0 and the reconciling change owns the bump, so that one branch moves
      the three lines once. All three lines must agree: `CMakeLists.txt`
      (`project(... VERSION)`), `bindings/c/clay.h`
      (`CLAY_ABI_MAJOR`/`MINOR`/`PATCH`) and `pyproject.toml` (`version`). If
      another branch has already taken 0.86.0, rebase and take the next number.
      No format minor: `kSceneMinor` does not move, because a convenience
      placement saves as the placement it produced.
- [x] 4.2 `python3 tools/check_c_abi.py` (header hygiene and the ctypes FFI) and
      `python3 tools/check_test_shards.py`.
- [x] 4.3 `npx -y @fission-ai/openspec@1.12.0 validate --all --strict` (CI repinned from 1.8.0 on 2026-09-06).
- [ ] 4.4 `python3 tools/release_check.py --skip-slow` before pushing.
- [x] 4.5 `docs/05-*` gains the three; `docs/RELEASE.md` at release time. Owned
      by the reconciling change alongside 4.1, so the library reference and the
      version lines move together. Landed as "Dropping a subtool on the floor
      (ABI 0.86.0)" in section 6, after the instancing section because the
      never-severed rule reads from it, plus the three names in the pyclay
      section. `docs/RELEASE.md` deliberately untouched.

## Deliberately not in this change

- [ ] 5.1 **`pick::layer_bounds` covering a radial layer's copies.** The
      influence path emits `radial_count - 1` rotated copies
      (`src/scene/bounds.cpp`); the pick path carries the mirror copies and
      stops, so `clay_layer_bounds` under-reports a radial layer and 1.4 refuses
      that case rather than answering it. Widening it changes what a
      camera-framing query answers for every existing host, and the
      mesh-rasterization-region scenario in the `c-abi` spec already pins the
      result, so it needs its own change and its own tests. The comment on
      `pick::probe_layer` reaches the same conclusion for the sibling probe
      path.
- [ ] 5.2 **Item-level variants.** `clay_layer_selection_bounds` and
      `clay_layer_set_transform_nonuniform` already give both halves, so this
      costs no re-layout later; it needs one answer this change does not:
      what a snap means for a selection spanning a group, whose children's
      placements are relative to it.
- [ ] 5.3 **Centring the PIVOT without moving the content.** A third reading of
      "centre", and not expressible at layer level: keeping the content still
      requires shifting every item's local placement by the inverse, which on
      an instance would move every other instance. Belongs with 5.2.
