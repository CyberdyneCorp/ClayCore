## 1. Measured before building

- [x] 1.1 Gap confirmed on `origin/main` `aafeccb6`: no `apply_to_dynamic`, no
      `clay_dynamic_sculptor_apply_stroke`, no `DynamicSculptor.apply_stroke`
- [x] 1.2 Host loop vs transplanted drag rules, `cube_sphere(24)`, detail 8:
      Snakehook reach 42% vs 96% (fixed: 50% vs 98%); Grab 56% vs 41% (fixed:
      66% vs 41%) — the adaptive result agrees with `apply_to_mesh` to 1e-3
- [x] 1.3 Snakehook anchor deaths under BEFORE+AFTER remesh: up to 14 in 61
      stamps at detail 4; never re-finding gives 15–18% reach; re-finding at the
      previous stamp position 81–98%, never worse than at the dead anchor's
      last position in sixteen rows
- [x] 1.4 Azimuth: surfaces bit-identical across azimuth 0 / pi/2 without
      `orient_alpha_by_stamp`, different with it
- [x] 1.5 Determinism with topology on: bit-identical reruns for Draw, Clay,
      Smooth, Grab, Snakehook, Flatten
- [x] 1.6 One `TopologyDelta` over a 20-stamp stroke reverts bit-identically
- [x] 1.7 C ABI per-stamp loop vs C++ loop: 1.001x — the entry point is not a
      latency change and the PR must not claim one

## 2. Engine

- [ ] 2.1 Make `DynamicSculptor::nearest_vertex` public, header comment stating
      it is the walk's seed estimator
- [ ] 2.2 Declare `brush::apply_to_dynamic` in `stroke.h` with what it does NOT
      promise: no normal deferral, Layer refused, `seed_class` not consulted,
      Grab's AFTER remesh at the first centre, cost = the sum of its stamps
- [ ] 2.3 Implement over `mesh_stamp_settings`, `mesh_mask_gate`,
      `mesh_automask_inputs`; Snakehook centre in `dynamic_snakehook_centre`
      (revalidate, re-find at the previous stamp position)
- [ ] 2.4 Refuse Layer, `defer_normals`, empty stamps before touching the
      surface, the record or the automask inputs
- [ ] 2.5 Accumulate the optional `DynamicStampResult` summary
- [ ] 2.6 Cognitive complexity of both new functions <= 15 (measure; state
      scores in the PR)

## 3. Engine tests (`tests/unit/test_dynamic_stroke.cpp`)

- [ ] 3.1 A stroke equals its stamps: for Draw, Clay, Smooth, Flatten and Grab,
      with taper, jitter and a pressure ramp, `apply_to_dynamic` on one surface
      and a hand loop of `DynamicSculptor::stamp` on an identical one are
      bit-identical, topology included; applied count equals the loop's
- [ ] 3.2 The remesh schedule runs per stamp: summary split/collapse counts
      equal the hand loop's summed `DynamicStampResult`s, and a Clay stroke with
      topology on splits before its first deposit (same counts as the loop)
- [ ] 3.3 Layer refused: returns 0, surface bit-identical, record empty,
      revisions unchanged
- [ ] 3.4 Azimuth reaches the alpha: rotate_to_azimuth + half-on alpha, azimuth
      0 vs pi/2 differ with `orient_alpha_by_stamp`, bit-identical without
- [ ] 3.5 Snakehook keeps pulling: pull-out reach >= 90% of the drag at detail 8
      (host-style following loop asserted < 60% in the same test so the
      assertion cannot pass vacuously); surface validates
- [ ] 3.6 Snakehook survives anchor death: detail 4 fixture where the anchor is
      retired at least once (assert the precondition by counting), reach >= 75%
- [ ] 3.7 One stroke is one undo step: whole stroke into one `TopologyDelta`,
      revert is bit-identical and validates
- [ ] 3.8 A mask gates the stroke: a fully masked stamp centre is skipped, a
      half-masked region moves on one side only
- [ ] 3.9 `defer_normals` refused: returns 0, surface bit-identical
- [ ] 3.10 Mutation: break the anchor re-find (never re-find) and the Grab
      anchor (follow the cursor); confirm 3.5/3.6 and 3.1 fail; restore

## 4. C ABI (0.117.0 -> 0.118.0)

- [ ] 4.1 Extract the topology descriptor decode from
      `clay_dynamic_sculptor_stamp` into one helper
- [ ] 4.2 `clay_dynamic_sculptor_apply_stroke` and `_apply_preset`, documented
      in `clay.h` beside `_stamp`: full samples and why, session frame only and
      why, no undo record, no deferral, Layer refused, cost is the stamps', the
      1.001x measurement
- [ ] 4.3 Accumulated `clay_dynamic_stamp_report` (sum, OR, union, final
      revision), honouring its `struct_size`
- [ ] 4.4 Bump `CMakeLists.txt`, `CLAY_ABI_MINOR`, `pyproject.toml`
- [ ] 4.5 `tools/check_c_abi.py` passes against a rebuilt `libclay_shared.dylib`
- [ ] 4.6 C tests in `test_c_dynamic_topology.cpp`: stroke equals the C
      per-stamp loop with the same resolved stamps; Layer refused with
      INVALID_ARGUMENT and nothing applied; declared world frame places the
      stroke; NULL/short report handled; apply_preset with the "Rake" preset
      differs across azimuth when oriented

## 5. Bindings

- [ ] 5.1 pyclay `DynamicSculptor.apply_stroke` / `.apply_preset`
- [ ] 5.2 pyclay test: stroke runs, Layer raises, azimuth column reaches the
      alpha
- [ ] 5.3 `check_binding_parity.py --pyclay <build>/bindings/python
      --require-import` prints `imported`, and passes
- [ ] 5.4 `tests/swift/smoke.swift` drives `_apply_stroke` and the Layer refusal

## 6. Docs and gates

- [ ] 6.1 `docs/07` §8b: the adaptive stroke, the anchor rule and its numbers
- [ ] 6.2 `docs/05`: the two entry points
- [ ] 6.3 `openspec/ROADMAP.md`: close the two rows naming the gap
- [ ] 6.4 Remove the "no `apply_to_dynamic`" statements from `stroke.h` and the
      estimator requirement (this change's REMOVED + ADDED delta)
- [ ] 6.5 Open the follow-up issue for fixed-path Grab anchoring with the 41% vs
      66% measurement
- [ ] 6.6 GCC -Werror flags reproduced where possible; `release_check.py
      --skip-slow` green; `openspec@1.12.0 validate --all --strict`
