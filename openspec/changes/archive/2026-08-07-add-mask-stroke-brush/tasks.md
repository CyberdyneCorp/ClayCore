# Tasks: add-mask-stroke-brush

- [x] 1.1 `MaskField::fill(Aabb, value)` and `invert_within(Aabb)`: a bounded
      complement, so "mask everything else" is expressible
- [x] 1.2 `brush::apply_to_mask`: the third stroke consumer, owning the
      world-radius → mask-cell conversion
- [x] 1.3 Accumulation for a lerp-toward-target field: what Buildup and Clamped
      mean when paint moves toward a target rather than adding
- [x] 1.4 `RelaxSettings::mask` and `FlattenSettings::mask`, weighted by
      `(1 - mask)` at the world position of the sample being written
- [x] 1.5 C ABI: `clay_mask_apply_stroke`, `clay_mask_fill`,
      `clay_mask_invert_within`, and the mask on the relax/flatten params
- [x] 1.6 Python bindings for all of the above
- [x] 1.7 Tests: a stroke paints evenly; the same stroke at two mask cell sizes
      covers the same world region; Clamped stops at the target and Buildup does
      not; relax and flatten leave a fully masked region bit-identical;
      `invert_within` is right across a chunk boundary
- [x] 1.8 `examples/11_masks.py` extended with a stroke-painted mask and a
      masked relax; docs and roadmap

Found while building:

- [x] 1.9 The field verbs take the mask as a CALLABLE rather than as a
      `MaskField`. Naming the type in `field/` is a layering violation the
      check caught: a sampled field is a leaf that sits BELOW `scene`, while a
      mask sits above it, so field -> voxel -> scene -> field is a cycle. What a
      verb actually needs is a scalar at a world point, and taking that is both
      honest and acyclic.
- [x] 1.10 `region_is_walkable`, found by a test that expected a refusal and got
      an accepted call. Guarding on `Aabb::is_infinite()` catches only the exact
      FLT_MAX sentinel; a box merely NEAR it is finite, and its cell indices
      overflow the lattice's own int32 long before its volume overflows a float.
      The count is now done in double, per axis before the product, and the C
      ABI turns the refusal into a typed error rather than a silent no-op.
- [x] 1.11 `parse_extrude_side` accepts one spelling per side. The parity gate
      is right to insist: every string pyclay accepts is a capability the C ABI
      has to be able to name, and "centered" beside "centred" would have been a
      second enumerator meaning exactly what the first one does.
