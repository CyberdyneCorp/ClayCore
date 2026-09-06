# Tasks: read-a-placed-node

- [x] 1.1 Decide what a rotation reads back as: the node stores a quaternion and the setter takes an axis and an angle, so the reader has to choose a representative — **w made non-negative**, which puts the angle in `[0, pi]` and picks between the two names every rotation has, and **+Y at angle 0** for a rotation with no axis, because `read_transform` refuses a zero axis and a reader its own setter rejects is not a round trip. `atan2(|v|, w)` rather than `acos(w)`, which loses its precision exactly where an unrotated item sits
- [x] 1.2 `clay_layer_node_transform`: position, rotation axis/angle, scale; a group refused as its setter refuses one; every out-pointer optional
- [x] 1.3 `clay_layer_node_params`: the size-query pattern, counted in FLOATS, with the arity the CURRENT primitive takes; a group refused as `clay_layer_node_prim` refuses one; the out-of-line kinds count 0 rather than refusing
- [x] 1.4 `clay_layer_node_op_blend`: op, blend, blend radius, rounding; answers for a group as well as an item, because both carry them
- [x] 1.5 Tests: `tests/unit/test_c_node_readback.cpp`, 7 cases — a round trip through each setter, the rotation fixed point and the turn past pi, a group's refusals and the op it does answer, the size-query and buffer-too-small paths, a stroke counting 0, the typed refusals, and a mirrored/hidden/ghosted/locked document reloaded and read while `clay_layer_node_influence_bound` reports a box centred on the origin
- [x] 1.6 Version: ABI minor to 0.53.0 in `CMakeLists.txt`, `bindings/c/clay.h` and `pyproject.toml` together — the three that have been left behind before
- [x] 1.7 Docs: `docs/05-claycore-library.md` and `docs/07-brushes-and-features.md` say the reading surface is now complete for a plain item, the feature table gains the three calls, and `docs/RELEASE.md` records 0.53.0 as additive
- [x] 0.1 SEQUENCING: no `.clayspace` or scene-minor bump — nothing new is stored, so this runs in parallel with any change that does not also take an ABI minor

## Deliberately not in this change

- [ ] 2.1 **A colour reader.** `clay_layer_set_color` is the fourth setter and
      still has no reader after this. #317 asks for three calls and names the
      four values its side-car holds; colour is not one of them. Adding the
      fourth is a one-line sibling of `clay_layer_node_op_blend` and should be
      its own issue rather than surface nobody asked for. The header says so at
      the call, so it is a recorded gap and not an oversight.
- [ ] 2.2 **The pyclay mirror.** Same readers, Python shapes. The parity gate
      runs pyclay -> C, so a C-only addition keeps it clean, but the two
      surfaces should not stay asymmetric for long.

## Found while doing it

- [x] 3.1 `clay_layer_stroke_points` and `clay_layer_children` count in points
      and in ids; this one counts in FLOATS, and the header says which at the
      call. A size-query family whose unit varies per call is a trap worth
      naming rather than assuming a reader will infer it from the buffer type.
