# Proposal: a sign per armature node

## Why

An armature carries one op for the whole item, so every node adds or every node
subtracts — ZBrush's negative ZSphere cannot be expressed (#99). Hosts fake it
by placing separate subtract spheres after the armature, and the fake has three
defects the reporter names precisely: the skin along the negative's links is
not cut (a deep hollow shows a bridge across its own opening), the sign does
not survive a save (a reopened rig comes back all-positive and can no longer be
un-negatived), and a negative has to be a leaf. The primitive is one field away
from expressing all three correctly.

## What Changes

- The armature item gains a per-node sign, positive by default, that travels
  with the tree: through the builder setter, the placed edits, the readback and
  the file — the round trip the parents already make.
- Evaluation is the armature of the positive nodes MINUS the armature of the
  negative nodes, each half built exactly as the unsigned armature is; a link
  whose ends disagree in sign belongs to neither half. Skin along a negative
  node's links is therefore never drawn (the membrane cut), a carve never
  sweeps a positive parent's radius (an eye-socket child does not swallow the
  head), and a negative parent is legal.
- C ABI, purely additive: `clay_item_set_armature_signs` mirroring the parents
  setter, `clay_layer_armature_signs` mirroring the parents readback, and a
  fifth tree edit `CLAY_ARMATURE_SET_SIGN`. No existing signature changes.
- The node record and `SetArmatureCmd` carry the signs at format minor 8,
  gated exactly as the parents were at minor 7.
- `pyclay` takes a `signs` array beside `parents`, exposes it read-write as
  builder state, and grows `armature_edit(op="set_sign")`; the armature example
  carves with negative nodes.

The negative-radius convention in `xyzr` (the issue's option 2) is rejected: it
legalises input the setter refuses today, and typed refusals are load-bearing
in this ABI.

## Capabilities

### New Capabilities

None — the sign is a property of the existing armature capability in each spec.

### Modified Capabilities

- `c-abi`: the armature requirement grows the signs setter, the signs readback
  (size-query pattern, counted in nodes, short-stored signs padding positive)
  and the `SET_SIGN` tree edit; refusals stay typed.
- `sdf-kernels`: the tree-of-spheres requirement states the signed fold —
  positive links ascending, then negative links ascending — and that an
  all-positive armature evaluates identically to today, preserving the
  chain-equals-stroke identity.
- `scene-model`: the tree-edit requirement gains set-sign (undoable, refused on
  a protected layer); persistence keeps bit-identical save/reload with signs.
- `file-io`: the node record carries the signs gated on minor 8; a pre-8 reader
  is unaffected the way a pre-7 reader was for parents.
- `python-bindings`: armatures take a signs array with C parity.

## Impact

- `bindings/c/clay.h` + `clay_c.cpp` (setter, readback, edit dispatch),
  `include/clay/scene/types.h` (`Node::armature_signs`),
  `src/scene/{armature,tape_build,commands}.cpp`, `include/clay/kernel/tape.h`
  (`ctape_armature_dist` signed fold), `include/clay/io/clayspace.h` +
  `src/io/clayspace.cpp` (minor 8), `bindings/python/pyclay_module.cpp`,
  `examples/40_armature.py`, `tools/check_binding_parity.py`.
- Tests: `tests/unit/test_armature.cpp`, `tests/unit/test_c_armature.cpp`,
  parity corpus scene in `tests/unit/test_parity.cpp`.
- Docs: `docs/05-claycore-library.md`, `docs/07-brushes-and-features.md` §6.
