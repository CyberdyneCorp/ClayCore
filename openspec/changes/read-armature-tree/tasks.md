# Tasks: read-armature-tree

- [x] 1.1 Confirm the tree already persists: parents ride the node record at format minor 7 and `SetArmatureCmd` replays them — exposure only, no format change
- [x] 1.2 C ABI: `clay_layer_stroke_points` accepts `CLAY_PRIM_ARMATURE` (the read side of `find_curve_node`; the placed-node SETTER stays narrower on purpose); `clay_layer_armature_parents` by the size-query pattern, counted in nodes, short-stored trees padded with roots exactly as `tape_build` reads them; `clay_layer_node_prim` refusing groups as the dual of `clay_layer_children`
- [x] 1.3 Parity gate: nothing to move — the gate is C-reaches-what-pyclay-reaches and pyclay's builder-side readers were already exempt as builder state
- [x] 1.4 Regression test (`tests/unit/test_c_armature.cpp`): the issue's exact branching rig (parents {0,0,1,1}, node 3 off 1 not 2) placed, saved, reloaded, found via `clay_layer_node_prim`, both halves read back exactly, re-posed via `clay_layer_armature_edit` through read-back indices with the branch (not the mis-guessed chain) carried; the typed refusals; the short-parents padding; the placed setter still refusing an armature
- [x] 1.5 Docs: `docs/05-claycore-library.md` placed-node readback paragraph, `docs/07-brushes-and-features.md` §6
