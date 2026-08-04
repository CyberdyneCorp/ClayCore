# Tasks: add-c-abi-item-builder

## 1. Foundations
- [ ] 1.1 `uint32_t struct_size` convention on descriptor structs; teach `tools/check_c_abi.py` to require it
- [ ] 1.2 Bump ABI to 0.2.0 in `clay.h`, `CMakeLists.txt`, `pyproject.toml` (the release gate checks agreement)
- [ ] 1.3 Enum sync: `clay_prim`/`clay_op`/`clay_blend` complete, values equal to tape opcodes, one static_assert per entry

## 2. Item builder
- [ ] 2.1 `clay_item_create`/`_destroy`; setters for primitive params, transform, op, blend, rounding, colour, mirror
- [ ] 2.2 Deformer chain setter preserving order; repetition setter
- [ ] 2.3 Profile setter incl. polygon vertices; stroke point list; transition parameters
- [ ] 2.4 `clay_layer_add_item`; redefine flat `clay_add_item` as sugar over the builder
- [ ] 2.5 Tests: composed edit equals a pyclay-authored document; deformer order; variable-length payloads; flat path unchanged; every primitive reachable

## 3. Close-out
- [ ] 3.1 Docs: `docs/05`, README, `docs/RELEASE.md`
- [ ] 3.2 Full verification: all presets, gates, release checklist, CI
