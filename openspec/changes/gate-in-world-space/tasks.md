## 1. The frame

- [x] 1.1 `emit_gate` writes an identity placement and unit scale instead of
      `layer.xform * item.xform` squashed by the item's per-axis scale
- [x] 1.2 Drop the `Layer` argument that `emit_gate` and `emit_combine` only
      carried to reach the item's placement

## 2. The cost

- [x] 2.1 `fold_gate` charges `gate_width` in world units, with no layer scale
- [x] 2.2 Delete `layer_scale_for_gate_`, which nothing reads any more

## 3. Proof

- [x] 3.1 A gate stays put when the gated item is MOVED along an axis its carve
      is invariant under
- [x] 3.2 ...when the item is TURNED onto its own footprint
- [x] 3.3 ...when the LAYER is turned onto its own footprint
- [x] 3.4 ...and when the item is SCALED
- [x] 3.5 Two documents describing the same world by different splits of scale
      between layer and item declare the same step scale, and marching the
      scaled one by its own number does not overshoot
- [x] 3.6 `tests/c_api/smoke.c` covers `clay_item_set_gate` at the boundary,
      with a PLACED item — it had no C-level coverage at all
- [x] 3.7 Each property fails when its own half of the fix is reverted

## 4. Say which frame it is

- [x] 4.1 `scene::Node::gate`
- [x] 4.2 `clay_item_set_gate`
- [x] 4.3 `Prim.gate`
