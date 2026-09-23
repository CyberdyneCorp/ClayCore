## 1. The gap, measured on origin/main (aafeccb6)

- [x] 1.1 `grep -c TopologyDelta bindings/c/clay.h` returned 0, and
      `clay_dynamic_sculptor_stamp` passes `nullptr` for the record
- [x] 1.2 The "known open defect" in `test_dyntopo.py` was the free-list cycle,
      fixed in `aae921fe` (#437). The comment is stale
- [x] 1.3 Revert without index maintenance: 200 / 2,638 live faces missing from
      the index, 0 dirty chunks, and the same stroke again is NOT bit-exact
- [x] 1.4 `rebuild_index` repairs the index, but clears the dirty set, renumbers
      chunks (88 -> 64) and costs 39 / 189 / 896 ms at 49k / 197k / 786k faces.
      Incremental maintenance costs 0.106 / 0.295 / 2.067 ms and is bit-exact on
      the repeat stroke
- [x] 1.5 The relax pass leaves the record's normals stale: redo gets 346
      vertex normals wrong, undo gets 14 wrong on one configuration and 2,910
      over 8 strokes. With `relax_after_remesh = false` both are 0
- [x] 1.6 An out-of-order revert fails `validate`, including between strokes on
      opposite hemispheres, which shared 322 reused slots
- [x] 1.7 Byte cost: the encoded size matched the fixed-width formula exactly
      (1,233,636). `bytes()` depends on the allocator (1,884,384 against a
      1,261,256-byte payload)

## 2. Fix the record: the relax pass notes what it rewrites

- [x] 2.1 Regression test first, in `tests/unit/test_topology_delta.cpp` or
      `test_dynamic_history.cpp`: an adaptive stroke with relax on. Assert that
      the record's `after` equals the live surface right after capture, and that
      undo and redo reproduce `to_mesh` normals at both ends
      Landed in a new `tests/unit/test_dynamic_replay.cpp` instead
- [x] 2.2 Confirm the test fails on the unfixed tree. It should report nonzero
      differing normals, matching measurement 1.5
      Done as a mutation: `remesh_local.cpp` taken from origin/main fails the
      vertex- and face-normal counts in `test_dynamic_replay.cpp` and the
      normals checks in `test_c_dynamic_delta.cpp`. The record's entry counts
      did NOT grow (see the proposal's "What building it found")
- [x] 2.3 `remesh_local.cpp` relax loop: collect the incident faces up front,
      note those faces and their vertices, write positions, `refresh_normals`,
      then sync them
- [x] 2.4 The test passes. Re-run `test_topology_delta` and `test_dynamic_*` to
      confirm the record's growth changed no other assertion

## 3. Engine: mark, recorded gesture, replay

- [x] 3.1 `DynamicSurface::mark()` / `set_mark()`. The lineage comes from an
      atomic counter mixed with a per-process seed. The epoch advances inside
      `bump_topology`, `bump_geometry` and `bump_attributes`. The mark is NOT in
      `DynamicSurface::encode`
- [x] 3.2 Audit every writer of the pools and confirm that each one bumps a
      revision. A writer that does not bump would slip past the guard
      No writer skips a bump: split, collapse, flip, the relax pass,
      `write_positions`, `write_colors`, `TopologyDelta::revert` / `apply`.
      `trim` touches only the chunk arena
- [x] 3.3 `RecordedGesture` holds `TopologyDelta` plus the before and after
      marks. Its encoding is `'CDGR'` v1 plus the marks plus the unchanged
      `CTDL` bytes, and its decode refuses a truncated buffer or an unknown
      version before allocating
- [x] 3.4 A const view of `TopologyDelta`'s face and vertex entries
- [x] 3.5 A `DynamicSculptor::stamp` overload taking `RecordedGesture*`. It
      refuses capture into a bound, non-empty record whose `after` mark differs
      from the surface
      Named `stamp_recorded`, not an overload: `stamp(..., nullptr)` at every
      existing call site would become ambiguous
- [x] 3.6 `DynamicSculptor::replay(const RecordedGesture&, Direction)`, split
      into `guard`, `restore` and `reindex`: erase face slots before the
      restore, insert live targets after it, refit faces around moved vertices.
      It returns Ok, NoOp or Mismatch
      The result is spelled `ReplayResult::Applied` rather than Ok
- [x] 3.7 Measure the cognitive complexity of `replay` and its parts; the target
      is 15 or less each
      clang-tidy: `replay` 4, `reindex_recorded_faces` 8,
      `refit_around_moved_vertices` 14, `RecordedGesture::decode` 9, `guard` 5;
      `relax_region` 39 (45 on origin/main)

## 4. C ABI (0.117.0 -> 0.118.0)

- [x] 4.1 Bump `CMakeLists.txt`, `CLAY_ABI_MINOR` and `pyproject.toml` together
      **ABI transition: 0.117.0 -> 0.118.0**
- [x] 4.2 `clay_dynamic_delta` with `clay_dynamic_delta_create`,
      `clay_dynamic_delta_destroy`, `clay_dynamic_delta_clear`,
      `clay_dynamic_delta_stats_get`, `clay_dynamic_delta_revert`,
      `clay_dynamic_delta_apply`, `clay_dynamic_delta_serialize` and
      `clay_dynamic_delta_deserialize`, plus
      `clay_dynamic_sculptor_stamp_recorded`
- [x] 4.3 `clay_dynamic_delta_stats` behind `struct_size`, with its original
      layout named by its last field
- [x] 4.4 Extract the descriptor reading shared by `clay_dynamic_sculptor_stamp`
      and `clay_dynamic_sculptor_stamp_recorded` into a helper, not a copy
- [x] 4.5 Error mapping:
      - a mismatch is `CLAY_ERROR_SNAPSHOT_MISMATCH`;
      - a short serialize buffer is `CLAY_ERROR_BUFFER_TOO_SMALL`, with the size
        needed;
      - a newer serialized version is `CLAY_ERROR_FORWARD_VERSION`;
      - unreadable bytes and null handles are `CLAY_ERROR_INVALID_ARGUMENT`
- [x] 4.6 Header text on what the calls do NOT promise:
      - `rebuild_index` is not needed, and calling it clears the dirty set and
        renumbers chunks;
      - serialized surface bytes change after an undo;
      - `dead_slots` grows;
      - replay is last-in first-out only;
      - a record never outlives its surface handle;
      - serialization is not crash recovery;
      - the memory ledger does not count records
- [x] 4.7 `tools/check_c_abi.py` passes against a freshly built
      `libclay_shared.dylib`. Update the struct mirror in the checker
      No struct mirror was needed: the checker holds none for the dynamic
      surface. Passes against the freshly built cpu-only libclay_shared.dylib

## 5. Tests (counts, not clocks)

- [x] 5.1 C ABI, new `tests/unit/test_c_dynamic_delta.cpp`:
      - undo gives byte-identical `to_mesh` (positions, normals, indices) and
        `validate` ok;
      - redo gives the post-stroke export;
      - N=8 strokes undone in reverse, then redone in order, match the export
        at every step
- [x] 5.2 The byte cost as a count. `encoded_bytes == 56 + 122V + 114H + 42E +
      66F` over the reported counts, and it equals the serialize size query.
      `resident_bytes >=` the entries times `sizeof`. `clear` reports zero
      entries
- [x] 5.3 Refusals:
      - an older record reverted under a newer one gives SNAPSHOT_MISMATCH, with
        revision and `to_mesh` unchanged;
      - an unrecorded stamp in between is refused;
      - a record from another surface is refused;
      - reverting twice gives CLAY_OK with no revision change
- [x] 5.4 The index and the chunk stream:
      - the same stroke after an undo equals the first stroke's export;
      - reassembly from dirty chunks after an undo equals `to_mesh`
- [x] 5.5 Mutate each claim before trusting it:
      - drop the reindex step: 5.4 must fail;
      - drop the guard: 5.3 must fail, or fail `validate`;
      - revert the relax fix: 5.1's normals must fail
- [x] 5.6 A serialize/deserialize round-trip reverts identically. Truncated
      bytes and a bumped version are both refused

## 6. Bindings

- [x] 6.1 pyclay:
      - `TopologyDelta` with `revert`, `apply`, `clear`, `stats`, `serialize`
        and `deserialize`;
      - `DynamicSculptor.stamp(record=)`;
      - a refusal raises `ValueError`
- [x] 6.2 `tools/check_binding_parity.py`: map `TopologyDelta` to the
      `clay_dynamic_delta_` prefix and its constructor, and `record` to
      `clay_dynamic_sculptor_stamp_recorded`. Run it with a BUILT pyclay and
      confirm the output says `imported`
      Output: `parity: OK (764 pyclay capabilities, 31 exempt, imported ...)`.
      The gate reads members, not keyword arguments, so `record=` <->
      `stamp_recorded` is held by the tests on both sides
- [x] 6.3 `bindings/python/tests/test_dyntopo.py`: replace the "UNDO/REDO IS
      DELIBERATELY ABSENT" paragraph with undo/redo parity and refusal tests
- [x] 6.4 `tests/swift/smoke.swift`: capture, revert, apply and stats beside the
      existing dynamic-surface block
      `check_swift_smoke.sh typecheck` passes; built and run against the
      cpu-only `libclaycore.a`, 432 of 433 checks pass, the one failure being
      the Metal-backend registration a cpu-only library cannot satisfy

## 7. Docs and gates

- [x] 7.1 `docs/05-claycore-library.md`: an adaptive-surface undo section
      covering host-owned records, last-in first-out replay, budgeting with
      `resident_bytes` and spilling with serialize
- [x] 7.2 The follow-up for `session::History` holding a stale sculptor index
      is recorded in the PR body, not fixed here
      Recorded in `design.md` (Follow-up) for the PR body; no PR is opened here
- [x] 7.3 Reproduce the Ubuntu GCC `-Werror` flags where possible, run
      `python3 tools/release_check.py --skip-slow`, and run
      `npx -y @fission-ai/openspec@1.12.0 validate --all --strict`
