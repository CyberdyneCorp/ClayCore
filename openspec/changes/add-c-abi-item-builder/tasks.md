# Tasks: add-c-abi-item-builder

## 1. Foundations
- [x] 1.1 `uint32_t struct_size` convention on descriptor structs; teach `tools/check_c_abi.py` to require it
- [x] 1.2 Bump ABI to 0.2.0 in `clay.h`, `CMakeLists.txt`, `pyproject.toml` (the release gate checks agreement)
- [x] 1.3 Enum sync: `clay_prim`/`clay_op`/`clay_blend` complete, values equal to tape opcodes, one static_assert per entry

## 2. Item builder
- [x] 2.1 `clay_item_create`/`_destroy`; setters for primitive params, transform, op, blend, rounding, colour, mirror
- [x] 2.2 Deformer chain setter preserving order; repetition setter
- [x] 2.3 Profile setter incl. polygon vertices; stroke point list; transition parameters
- [x] 2.4 `clay_layer_add_item`; redefine flat `clay_add_item` as sugar over the builder
- [x] 2.5 Tests: composed edit equals a pyclay-authored document; deformer order; variable-length payloads; flat path unchanged; every primitive reachable

## 3. Close-out
- [x] 3.1 Docs: `docs/05`, README, `docs/RELEASE.md` — incl. the 0.2.0 binary
      break in `clay_item_desc` / `clay_mesh_params` and the 0.x SemVer rule
- [ ] 3.2 Full verification: all presets, gates, release checklist, CI
