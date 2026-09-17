ABI 0.119.0 -> 0.120.0.

## 1. Measured before building

- [x] 1.1 Gap confirmed on origin/main at `a60d8c16`: `apply_to_dynamic` takes a
      raw `TopologyDelta*` (no marks), the C stroke calls and pyclay take no record
- [x] 1.2 C host loop of `stamp_recorded` vs the C stroke, `cube_sphere(16)`:
      Draw identical (664,260 encoded bytes both); Snakehook and Grab surfaces
      differ (1,107,948 vs 1,205,836 and 1,069,644 vs 448,280 bytes)
- [x] 1.3 Mark checked once: 0 `stamp_recorded` refusals at k>0 over 97 stamps
      on seven fixtures; mark moved iff the stamp changed; mutation (one
      unrecorded stamp before k=3) fires at k=3
- [x] 1.4 Accumulation: dab + Clay stroke + Grab stroke into one record
      (707,358 -> 1,263,002 -> 1,647,616 bytes), one revert to pristine, one
      apply to the end, both exact
- [x] 1.5 Exactness with relax on for Draw, Clay, Smooth, Flatten, Grab,
      Snakehook and the anchor-death Snakehook (11 deaths in 61 stamps): undo and
      redo bit-exact over the export and stored normals of live elements,
      `validate` ok, index covers every live face; surface == unrecorded
      stroke; encoded bytes == per-stamp recorded loop in every row
- [x] 1.6 Refusals (Layer, defer_normals, empty, unrecorded stamp between,
      another surface, all-miss stroke): record size and marks unchanged
- [x] 1.7 Cost: 1.046x / 1.065x at 27,648 / 110,592 faces vs 1.049x / 1.061x
      for the per-stamp recorded loop (14 Draw stamps, median of 61 / 21)

## 2. Engine

- [x] 2.1 Declare `brush::apply_to_dynamic_recorded` in `stroke.h` with what it
      does NOT promise (mismatch = nullopt, nothing stamped, record untouched;
      refusals return 0 before the mark check; checked once; accumulates into a
      non-empty record; an all-miss stroke into an empty record re-binds it)
- [x] 2.2 Implement in `stroke.cpp` over `reset_dynamic_summary`,
      `dynamic_stroke_refused`, `can_capture_on` / `begin_capture` /
      `end_capture` and `apply_to_dynamic`
- [x] 2.3 `apply_to_dynamic`'s header: `record` is the UNGUARDED delta; point at
      the sibling
- [x] 2.4 Cognitive complexity of new and touched functions <= 15 (measure;
      state scores in the PR)

## 3. Engine tests (`tests/unit/test_dynamic_stroke.cpp`)

- [x] 3.1 Recorded stroke == unrecorded stroke (export, stored normals, applied
      count, summary) for Draw, Clay, Smooth, Flatten, Grab, Snakehook
- [x] 3.2 Undo/redo through `DynamicSculptor::replay` exact for the same verbs
      with relax on, plus the anchor-death Snakehook with deaths >= 1 REQUIRED;
      `validate` and index coverage at both ends
- [x] 3.3 Recorded stroke's encoded size == a per-stamp `stamp_recorded` loop
      with the stroke's rules (every `stamp_recorded` REQUIREd to succeed)
- [x] 3.4 Accumulation: stamp + stroke + stroke into one record, one revert to
      pristine, one apply to the end
- [x] 3.5 Refusals leave the record untouched: Layer, defer_normals, empty
      stamps, mismatched after an unrecorded stamp, record from another surface
      (record encoded size and both marks, surface revisions)
- [x] 3.6 Mutate: (a) skip `begin_capture` -> 3.4 or 3.2 fails; (b) drop the
      mark check -> 3.5's mismatch rows fail; (c) move the mark check after the
      stroke -> 3.5 fails on surface; record what failed in "What building it
      found": (a) fails 0 of the stroke tests and 4 assertions of the added
      LIFO case; (b) fails 10; (c) fails 6

## 4. C ABI

- [x] 4.1 `clay_dynamic_sculptor_apply_stroke_recorded` and
      `clay_dynamic_sculptor_apply_preset_recorded` in `clay.h` with the header
      block: what it does, NULL record, mismatch last and retryable, refusals
      untouched, accumulation, measured cost (design D6), and what it does not
      promise
- [x] 4.2 Implement in `clay_c.cpp`: `apply_dynamic_stroke` takes the record;
      the shipped calls forward with NULL; the check order of design D4
- [x] 4.3 Correct the `NO UNDO RECORD` bullet beside
      `clay_dynamic_sculptor_apply_stroke` to point at the recorded siblings
- [x] 4.4 Move the version lines: `CMakeLists.txt`, `clay.h`
      (`CLAY_ABI_MINOR 120`), `pyproject.toml`
- [ ] 4.5 `tools/check_c_abi.py` passes against a rebuilt `libclay_shared`
      (update its mirror only if it lists signatures)

## 5. C ABI tests

- [x] 5.1 A recorded ABI stroke equals the unrecorded ABI stroke (export) and
      reverts/applies exactly through `clay_dynamic_delta_revert` / `_apply`,
      `clay_dynamic_surface_validate` at both ends — Draw and Snakehook
- [x] 5.2 `_apply_preset_recorded` records a preset stroke that reverts exactly
- [x] 5.3 Mismatch: record, unrecorded stamp, recorded stroke ->
      `CLAY_ERROR_SNAPSHOT_MISMATCH`, `out_applied == 0`, export and revision
      unchanged, delta stats unchanged
- [x] 5.4 Layer with a stale record -> `CLAY_ERROR_INVALID_ARGUMENT` (not the
      mismatch); short report -> `CLAY_ERROR_INVALID_ARGUMENT`; both leave delta
      stats unchanged
- [x] 5.5 NULL record behaves as `clay_dynamic_sculptor_apply_stroke`
- [ ] 5.6 Swift smoke: recorded stroke, revert, apply, validate

## 6. pyclay

- [x] 6.1 `record=` on `DynamicSculptor.apply_stroke` and `.apply_preset`;
      mismatch raises `ValueError`, applies nothing; docstrings drop "No undo
      record"
- [x] 6.2 `test_dyntopo.py`: recorded stroke reverts/applies to the exports with
      `validate()`; a mismatched record raises and the revisions are unchanged
- [x] 6.3 Comment beside `"TopologyDelta.stats"` in `check_binding_parity.py`
      names the new pairing; run the gate against a BUILT pyclay and read the
      `imported <path>` line

## 7. Docs and specs

- [x] 7.1 `docs/05` "Stroking an adaptive surface": the recorded calls replace
      "captured only ... one stamp at a time"
- [x] 7.2 `docs/07` "A whole stroke" (§8b "Not provided"): same correction, and
      the C++ sibling
- [x] 7.3 `openspec/changes/stroke-an-adaptive-surface/specs/c-abi/spec.md`:
      correct the unarchived sentence "a `clay_dynamic_delta` is captured only
      per stamp"
- [x] 7.4 `openspec/ROADMAP.md`: record the closed gap on the adaptive stroke
      row (no row tracks it today; add to the `stroke-an-adaptive-surface`
      closure note)
- [x] 7.5 Fill "What building it found" in the proposal

## 7b. Verify

- [ ] 7b.1 `clay_unit_tests` full, sharded, in the background; counts in the PR
- [ ] 7b.2 `python3 tools/release_check.py --skip-slow`; diff any failure
      against origin/main (known machine-level: `dialect`, `device`)
- [ ] 7b.3 `npx -y @fission-ai/openspec@1.12.0 validate --all --strict`
- [ ] 7b.4 PR body: why, what lands, what measuring refuted, suite counts, gates,
      ABI 0.119.0 -> 0.120.0
