# Tasks

## 1. The field and the composition

- [x] 1.1 `Layer::scale_axes` beside `Layer::xform`, NOT a widening of
      `math::Transform` — it is a similarity and its algebra is closed only
      while it stays one.
- [x] 1.2 One place each for the composed map, its inverse and the conservative
      distance factor, beside the node-level helpers, so tape_build, bounds,
      pick and the brushes cannot drift on the order.
- [x] 1.3 tape_build: the inverse carries both scales, the scale slot carries
      the product of the two minima, rounding takes the same factor, and
      `is_exact` drops when either is non-uniform. A uniform triple compiles
      bit-identically.
- [x] 1.4 bounds and pick read the three factors, including the mirror and
      radial copies' own composition.

## 2. The brushes

- [x] 2.1 `brush::move`: centre and displacement through the composed inverse,
      radius divided by the largest factor of BOTH scales.
- [x] 2.2 `brush::lattice_gizmo` REFUSES a per-axis-scaled layer rather than
      placing a cage through a record that cannot hold its map, and says the
      scale is why.

## 3. Format

- [x] 3.1 `kSceneMinor` and `kClaySpaceMinor` to 16; both new fields gated on
      the writer's minor as the radial fields are.
- [x] 3.2 An older stream loads with the identity scale and the rigid cage map.
      Round-trip both ways.

## 4. C ABI

- [x] 4.1 `clay_document_set_layer_transform_nonuniform` and
      `clay_document_layer_transform_nonuniform`; the single-factor reader
      refuses a squashed layer.
- [x] 4.2 Tests: uniform is bit-identical; a squashed layer's shape, bounds and
      raycast; the narrow reader's refusal; a drag on a squashed layer; a cage
      on a squashed layer and on a squashed node; format round-trip.

## Deliberately not in this change

- [ ] 5.1 **`Deformer::cage_xform` as a general affine map.** The map a cage
      needs is `cage.placement⁻¹ · layer.xform · diag(L) · node.xform ·
      diag(N)`, and the record holds a `math::Transform`. `drag-a-squashed-item`
      deferred this here on the reading that the map needed was `Transform ∘
      diag`; with the LAYER scale in the composition it is a general affine map,
      which is a wider record and its own format question. Two gaps wait on it:
      a gizmo over a squashed layer (refused by 2.2 above) and the standing one
      where a cage is wrong inside a node that already carries a per-axis scale.
      Refusing is not a fix for the second — it predates this change and is
      untouched by it — so this stays open with the widening, not with the
      layer scale.
