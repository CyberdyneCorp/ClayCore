## Engine
- [x] `brush::StrokeTransaction`: `settled()`, `revised_from()`, `finish()`. A finished transaction takes no samples. `resolve_stroke_settled` reports the settled count, and `resolve_stroke` is it with the count dropped.
- [x] `brush::StampCursor`: each settled stamp handed out once, in order.
- [x] `apply_to_grid(..., first_index)`: the dither seed is the stamp's index in the stroke.
- [x] `MeshStrokeGesture`, `MultiresStrokeGesture` and `DynamicStrokeGesture`, with `apply_to_mesh`, `apply_to_multires` and `apply_to_dynamic[_recorded]` rebuilt as one gesture fed once.
- [x] `sample_path` resumes its segment search from the previous station.
- [x] `resolve_stroke_settled` split into `station_stamp`, `respond_to_speed`, `facing` and `jitter` (cognitive complexity 34 -> 15; it was 28 on main).

## C ABI
- [x] `clay_stroke_tx` and `clay_stroke_tx_status` in `bindings/c/clay.h`, with the settle rule, the binding rule, what a session holds open and what it does not do, all stated beside the calls.
- [x] The six `*_apply_stroke_tx` consumers. A later sculptor call's descriptors are decoded and compared field by field with the bind's (`same_sculpt_arguments`). `add_stroke_nodes` and `write_multires_report` are shared with the whole-path calls; `write_multires_report` is already registered as a bounded fill in `tools/check_c_abi.py`.
- [x] C ABI 0.125.0 -> 0.126.0 in `CMakeLists.txt`, `bindings/c/clay.h` and `pyproject.toml`.

## Bindings
- [x] pyclay `StrokeTransaction`. `tools/check_binding_parity.py` maps it to `clay_stroke_tx_`, aliases `status` to `clay_stroke_tx_status_get`, and lists the consumers as a C-only follow-up.

## Tests
- [x] `tests/unit/test_stroke_settle.cpp`: the settle property for ten presets × five schedules, the end taper, the lone sample, the station past the end, and the cursor.
- [x] `tests/unit/test_c_stroke_session.cpp`: stamps equal the whole path's for ten presets × five schedules. The handle's contract. Each of the six consumers equals its whole-path call, with one undo step. A grab per call differs. A start taper applies at lift. The binding refusals. A mismatched record refusal that loses no stamps.
- [x] `bindings/python/tests/test_stroke_transaction.py`.
- [x] Mutation check: eight injected defects, each caught.

## Docs
- [x] `docs/07-brushes-and-features.md`: the session, its rule, and the cross-binding table.

## Follow-up before 0.126.0 is tagged
- [x] `clay_multires_sculptor_apply_stroke_tx` takes a nullable `clay_multires_delta* record`, after `defer_normals` as in `clay_multires_sculptor_apply_stroke_recorded`. Changed in place: 0.126.0 is not tagged, so the ABI line stays 0.126.0. The record is part of the binding, is continued across the gesture's calls (both halves), and is checked, with the report, before the session's stamps are taken.
- [x] `tests/unit/test_c_multires_delta.cpp`: a session-fed stroke records the whole-path call's record and reverts bit for bit, with and without an active pass; a NULL record stamps the same; the record is part of the binding; a refusal (another pass active, a malformed report) loses no stamps. Mutation check: ignoring the record, checking it after the stamps are taken, dropping it from the binding, and checking the report afterwards each fail a new case.
- [x] `docs/05`, `docs/07` § 5 and § 8d.
