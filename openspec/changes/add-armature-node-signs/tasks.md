# Tasks: add-armature-node-signs

## 1. Scene model and kernel

- [x] 1.1 `Node::armature_signs` (`std::vector<std::int8_t>`, `include/clay/scene/types.h`) beside `armature_parents`; `armature_delete_subtree` and the other pure tree ops in `src/scene/armature.cpp` carry signs under renumbering; `armature_is_valid` unchanged (signs are not topology)
- [x] 1.2 `ctape_armature_dist` (`include/clay/kernel/tape.h`) grows a `signs` pointer and evaluates positive-armature-minus-negative-armature in two ascending passes: each half built by the unsigned rules (links between same-sign pairs only, a node whose parent has the other sign reading as a root of its half, referenced-root suppression per half), positives with the existing smooth union (bit-identical path when no node is negative), negatives via `-csmin_quadratic(-d, seg, k)` / hard `cmax(d, -seg)`; dispatch reads `prim_params[4]`
- [x] 1.3 `tape_build.cpp` writes signs to the blob (+1.0f/-1.0f, short arrays padded positive, exactly as short parents pad to roots) and emits 5 prim params; `bounds.cpp` untouched (subtraction only shrinks)

## 2. Persistence at minor 8

- [x] 2.1 `kClaySpaceMinor`/`kSceneMinor` 7 → 8 with the `static_assert` intact; node record writes `u32 count` + sign bytes gated `minor >= 8`, reader bounds-checks like the parents reader; `SetArmatureCmd` write/read/apply carry signs gated the same way, `apply_one` still refusing non-armature nodes and invalid trees
- [x] 2.2 Minor-8 compatibility note in `include/clay/io/clayspace.h` beside the minor-7 note (same trade: scene-payload growth, a pre-8 build fails rather than misreads, writing at minor 7 is the escape hatch and drops only signs)

## 3. C ABI

- [x] 3.1 `clay_item_set_armature_signs(item, const int8_t*, size_t)` mirroring the parents setter, refusing null/zero and any value other than ±1; `CLAY_ARMATURE_SET_SIGN 4` in `clay_layer_armature_edit`, sign in the `radius` argument (±1.0f, else refused), one `SetArmatureCmd`, protected-layer refusal
- [x] 3.2 `clay_layer_armature_signs` by the size-query pattern: null buffer answers the count, short buffer yields `CLAY_ERROR_BUFFER_TOO_SMALL` with the needed count and writes nothing, short-stored signs pad to +1, non-armature refused invalid-argument, readable on protected/hidden layers
- [x] 3.3 Doc comments in `clay.h` for all three surfaces, following the parents readback's doc block

## 4. Python bindings and example

- [x] 4.1 `Armature(nodes, parents=None, signs=None, ...)` with ±1 validation in a `to_signs` helper beside `to_parents`; read-write `signs` property; `Layer.add` copies signs to the placed node; `armature_edit(op="set_sign", node=..., target=..., sign=...)`
- [x] 4.2 `tools/check_binding_parity.py` rows for the signs surface
- [x] 4.3 `examples/40_armature.py`: negative-node section — eye sockets carved on the blocked-out figure by flipping two nodes, rendered beside the all-positive rig; example index line in `01_primitives.py` untouched

## 5. Tests

- [x] 5.1 `tests/unit/test_armature.cpp`: membrane cut (A–B(-)–C chain draws no sleeve through the hollow, vs the all-positive sleeve); a negative child carves without eating its positive parent (the eye socket); a negative parent-child pair carves its link as one swept scoop while positive relatives survive; all-positive rig evaluates bit-identically to the pre-signs path; all-negative rig is empty and not degenerate; signed round trip reserialises byte-identical; writing at minor 7 reproduces pre-signs bytes; set-sign edit is one undoable command; bad signs refused by the command; delete keeps survivors' signs under renumbering
- [x] 5.2 `tests/unit/test_c_armature.cpp`: signs setter/edit/readback typed refusals (0 and ±2, stroke, short buffer with needed count, protected layer refusing the edit but not the read); save → reload → signs read back → flip positive restores the field; negative node with descendants accepted and moved with subtree
- [x] 5.3 `tests/unit/test_parity.cpp`: `armature_negative` corpus scene (branching rig, one negative interior node, one referenced negative root) pinning the two-pass fold across registered backends
- [x] 5.4 Python: negative-armature parity case (pyclay vs C ABI evaluate identically) and ±1 validation, in `bindings/python/tests/`

## 6. Docs and PR

- [x] 6.1 `docs/05-claycore-library.md` armature paragraphs and `docs/07-brushes-and-features.md` §6: the sign, the membrane-cut semantics, the minor-8 note
- [x] 6.2 PR referencing #99; note the archive-order dependency on `read-armature-tree` (both deltas modify the c-abi armature requirement)
