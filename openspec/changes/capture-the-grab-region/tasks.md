## 1. Measured before building

- [x] 1.1 Issue #620 reproduced on `origin/main` at `2880560c`, ABI 0.120.0:
      `cube_sphere(24, 1.0)`, radius 0.3, spacing 0.1, 0.6 pull-out over 11
      stamps, 10 of 11 moving a vertex — reach **41.10%** fixed, **41.34%**
      adaptive, against **65.84%** / **55.82%** for a following centre. The
      issue's 41 / 41 / 66 / 56 to the point.
- [x] 1.2 The number is the mechanism's, not the fixture's: flipping the drag's
      sign (push-in) moves A by 0.06 points and mirroring the fixture to the
      other pole by 0.00; C by 0.00 and 0.49. Re-checked over grid (16/24/32)
      and spacing (0.25/0.1/0.05): A holds at 40.67–43.23% and 18.01–18.46%.
      A does NOT hold over the brush radius — 22.66% at 0.15, 41.10% at 0.30,
      60.56% at 0.50 — and C does not hold over the spacing: 16.41% / 65.84% /
      94.49% on one drag. What is invariant for A is the decay; what is
      invariant for C is that it loses the mesh (2 of 5, 11 of 26, 40 of 51
      stamps applied)
- [x] 1.3 A decays rather than discounts: 41% at a 0.6 drag, **18% at 1.5**
- [x] 1.4 Cross-representation agreement |fixed − adaptive|: A 3.2e-4 – 5.4e-3;
      B (remesher as today) **up to 3.4e-1**; C **up to 6.2e-2**; B with the
      remesher unable to touch the set **0.0 – 2.0e-5**
- [x] 1.5 Why B is fixture-dependent, counted: 7–13 of 45 captured vertices
      still live after 11 stamps; reach follows the largest surviving weight
- [x] 1.5b And 44% is not the floor: at radius 0.15 against detail 4 the
      remesher retires ALL 9 captured entries inside one stroke and unmaintained
      B reaches 2.81–6.97%, BELOW today's 9.34–22.30% on the same fixtures. At
      grid 32 / spacing 0.05 the push-in reads 29.02%. An unmaintained captured
      set can be a regression, not a weaker fix
- [x] 1.6 Control — topology OFF: B and C are BIT-EXACT across the two
      representations (0.000000), A is not (1.6e-3, 5.4e-3). The deformation
      rule is not what breaks the agreement; the remesher is
- [x] 1.7 Control — remesh held off inside the gesture, one at the tip
      (candidate Q): 100% reach on both paths on all six fixtures, agreement
      1.0e-6 – 2.0e-5, surface validates, 43–45 of 45 captured entries live
- [x] 1.8 C refuted a second time: 11 of 26 stamps applied on a 1.5 drag on both
      paths (A and B apply 25), reach 26% — the brush loses the mesh
- [x] 1.9 Cost, median of 200 strokes (fixed) / 50 (adaptive), Release, first run
      of each arm discarded, two independent processes agreeing to 11%: B is
      **1.85x cheaper than A on the fixed path** (0.145 → 0.077 ms; 0.311 →
      0.169 ms) and the same on the adaptive path (61.9 vs 60.3 ms), where the
      remesh dominates
- [x] 1.10 The 101x following-centre cost from the SDF move-coalescing path does
      NOT appear here: C is cheaper than A on both paths (0.086 vs 0.145 ms;
      31.7 vs 60.3 ms), and cheaper for the wrong reason — its region shrinks as
      it loses the surface. Checked and refuted
- [x] 1.11 The AFTER remesh must move: longest edge after a 1.5 pull-out is
      0.1174 under A, **1.1227** under B with the remesh left at the anchor
      (worse than not remeshing), 0.3571 with it following, 0.8176 under Q
      (exactly the fixed mesh's)
- [x] 1.12 Probe validity: with the probe patch applied and its switch at the
      default, the FULL unit suite passes **2839 of 2839 cases and 17,931,111 of
      17,931,111 assertions** — the scaffolding is inert at mode A, so every
      difference below is the rule and not the patch. The first two probe
      spellings were wrong and were caught by the counters, not by the eye: a
      `getenv` cached in a function-local static made all four modes
      byte-identical (`vertices_considered` equal across modes was the tell),
      and a re-gather on Q's last stamp applied the accumulated drag twice and
      read 180% reach

## 2. Engine — the captured region

- [ ] 2.0 **MEASURE THE MAINTENANCE BEFORE BUILDING THE REST.** 100% reach and
      the 2e-5 agreement belong to candidate Q, not to the recommendation, which
      does not exist yet; the step from one to the other assumes the maintenance
      keeps a HIGH-WEIGHT entry alive through the collapses, and D2's rules
      cannot create a weight above the surviving maximum. Build 3.1 FIRST, on
      its own, and report, on the fixtures of 1.5b (radius 0.15 / detail 4,
      where the unmaintained set goes to zero, and grid 32 / spacing 0.05):
      entries carried, inserted by a split, retired by a collapse, the top
      surviving weight at every stamp, and the reach. If the top surviving
      weight still falls away, say so and take one of the two named
      alternatives — protect a carried entry from collapse for the gesture, or
      `design.md` D3 — rather than carrying on. This is the change's one
      unverified inference and it is cheap to settle

- [ ] 2.1 A gesture-scoped captured region in `MeshSculptor`: the items, their
      captured positions and their weights, gathered once and reused, with the
      header stating that the write is `captured + w * total` and therefore
      carries no accumulation error
- [ ] 2.2 `mesh_stamp_settings` gives Grab `direction = s.position - first` and
      `center = first`, in the one place all three consumers share
- [ ] 2.3 `apply_to_mesh` opens the capture on the first unmasked stamp and
      closes it at the end of the call; a stroke that applies nothing captures
      nothing
- [ ] 2.4 `apply_to_multires` takes the same path with no new code; assert that
      a level re-bound mid-stroke does not silently drop the capture
- [ ] 2.5 Cognitive complexity of both consumers measured and stated in the PR

## 3. Engine — the adaptive surface

- [ ] 3.1 `DynamicSculptor` carries the captured region across its own remesh: a
      split inside the set inserts its new vertex at the midpoint of its
      parents' CAPTURED positions with the mean of their weights; a collapse
      removes the retired entry
- [ ] 3.2 The Grab remesh centre follows the stamp rather than staying at the
      first stamp's; `stroke.h` and `clay.h` lose the "runs around the first
      stamp's centre" caveat and gain what replaced it
- [ ] 3.3 Counters for the maintenance: entries carried, inserted by a split,
      retired by a collapse. The tests gate these counts, not a duration
- [ ] 3.4 `validate_dynamic_surface().ok` after every fixture in §4

## 4. Tests

- [ ] 4.1 Reach: a 0.6 pull-out Grab reaches the whole drag on the fixed mesh and
      on the adaptive surface, and the two agree to 1e-3 — the #619 constraint,
      re-asserted rather than dropped. Headroom 2.0e-5 — measured for candidate
      Q, which shares the deformation rule, NOT for the maintenance; 2.0 is the
      number to re-derive under 2.0 before this threshold is trusted
- [ ] 4.2 The same assertion with the drag's sign flipped and on the mirrored
      pole, so the threshold cannot be met by one fixture's luck
- [ ] 4.3 A curved drag and a 1.5 drag, both paths, both reaching the drag.
      **On the curve, assert the NET DISPLACEMENT and not the path length**: a
      quarter arc of length 0.6 has a chord of 0.5402, `kernel_grab` is handed
      `p_n − p_0`, and a case that demands 0.6 is demanding 111% of what the rule
      promises. Measured, B carries the region to 0.5402 of 0.5402
- [ ] 4.4 The maintenance is asserted as a COUNT: over the 1.5 pull-out the
      captured set is maintained rather than decaying — assert live entries at
      the end against entries at capture plus splits minus collapses, and assert
      the precondition (splits > 0) so the case cannot pass on a surface that
      never remeshed
- [ ] 4.5 Mutate before trusting: revert 3.1 and watch 4.4 fail; revert 3.2 and
      watch the longest-edge case fail. A test that passes with the fix reverted
      is not a test
- [ ] 4.6 A Grab through `apply_to_multires` — the first in the tree

## 5. The goldens that move, and what each becomes

Measured by running the suite under each candidate rule, not predicted. Under B
exactly two cases fail, both in one file; under C a third does.

- [ ] 5.1 `tests/unit/test_dynamic_stroke.cpp:196` "a stroke equals its stamps,
      topology included" — its `hand_loop` (lines 134–153) spells out today's
      rule (`b.center = stamps.front().position`, direction = motion between
      stamps). It is a MIRROR of the rule, not a pin on 41%: update the loop to
      the captured-set rule and the case means what it meant. 6 assertions,
      verb 0
- [ ] 5.2 `tests/unit/test_dynamic_stroke.cpp:737` "a recorded stroke is the
      unrecorded stroke, and undoes exactly" — same cause in `recorded_loop`
      (lines 663–690). 2 assertions, verb 0
- [ ] 5.3 `tests/unit/test_mesh_sculpt.cpp:727` "grab anchors and snakehook
      walks" — `CHECK(reach(Snakehook) > reach(Grab) + 0.1f)`. It measures which
      vertices were touched, not how far they moved, so the captured set leaves
      it PASSING (a captured region is still the region around the first stamp)
      and a following centre breaks it. Keep it; add the how-far assertion
      beside it, since the pair is what says a grab anchors AND pulls
- [ ] 5.4 `tests/unit/mesh_sculpt_goldens_{linux_x64,macos_arm64,msvc_x64}.inc`
      are NOT affected and must not be regenerated: `run_case` in
      `test_mesh_sculpt_parity.cpp` drives `MeshSculptor::stamp` directly with
      an explicit centre per step and never the stroke consumer. Verified by
      running the parity case under all five rules — it passes under every one.
      A PR that regenerates them has changed something it did not mean to
- [ ] 5.5 Nothing else in the seventeen files that touch a stroke consumer or
      Grab moves: `test_multires_sculpt.cpp`, `test_c_mesh_sculpt.cpp`,
      `test_c_dynamic_topology.cpp`, `test_c_dynamic_delta.cpp`,
      `test_c_stroke.cpp`, `test_sculpt_allocation.cpp`, `test_stamp_frame.cpp`,
      `test_dynamic_replay.cpp`, `test_dynamic_history.cpp`,
      `test_sculpt_kernels.cpp`, `test_dynamic_shared_brush_parity.cpp`,
      `test_multires_shared_brush_parity.cpp` all pass under every rule
- [x] 5.6 Confirmed on the FULL suite and not only the targeted shard: under the
      captured-set rule it is **2837 of 2839 cases, 8 of 17,931,111 assertions
      failing**, and they are exactly 5.1 and 5.2; under a following centre
      **2836 of 2839, 9 assertions**, adding exactly 5.3. Nothing else in the
      tree moves under either rule. Re-run independently, with the captured-set
      rule made the DEFAULT in all three consumers rather than switched on:
      2839/2839 and 17,931,111 assertions at `origin/main`, then 2837/2839 and
      17,931,103, the eight failures being `test_dynamic_stroke.cpp:224, 228,
      229, 230, 231, 232, 778, 779` and nothing else. 5.4 re-checked by reading:
      `run_case` sets `s.center` per step and calls `MeshSculptor::stamp`, so no
      stroke consumer and no capture reaches it
- [ ] 5.7 Still to run before the PR: the pyclay tests from a build that actually
      has pyclay (the cpu-only preset does not enable it), the Swift surface, and
      the device suite, none of which this measurement could reach

## 6. Documentation and spec

- [ ] 6.1 `include/clay/brush/stroke.h` — the Grab sentence at `apply_to_mesh`,
      the adaptive one at `apply_to_dynamic`, and the "Grab's AFTER remesh runs
      around the first stamp's centre" caveat
- [ ] 6.2 `bindings/c/clay.h` — the same two places beside
      `clay_mesh_sculptor_apply_stroke` and `clay_dynamic_sculptor_apply_stroke`
- [ ] 6.3 `docs/07-brushes-and-features.md` — the Grab paragraph, the "Not
      provided" note about the after-remesh, and the "known question left open"
      paragraph, which this change closes with numbers
- [ ] 6.4 `docs/07` also states, beside both, that the SDF move brush still moves
      less than asked and why the two now differ
- [ ] 6.5 `CLAY_ABI_*` and `kSceneMinor` do not move; no pyclay or Swift
      SIGNATURE moves. pyclay's DOCSTRINGS do:
      `bindings/python/pyclay_module.cpp:5671` ("'grab' anchors on the first
      stamp and drags by the motion between stamps") and `:8682` ("'grab'
      centres on the first stamp"). So does
      `include/clay/brush/stroke.h:383`, the `apply_to_mesh` sentence itself,
      which 6.1's list omitted
- [ ] 6.6 Release notes: the reach changes from 41% to the whole drag, which is
      5.6x further on a 1.5 drag, and any host feel tuned against 41% changes

## 7. Verification

- [ ] 7.1 `cmake --preset cpu-only -DCLAY_BUILD_TESTS=ON` + full `ctest`
- [ ] 7.2 `python3 tools/check_layering.py`, `check_kernel_dialect.py`,
      `check_licenses.py`, `check_c_abi.py`, `check_test_shards.py`,
      `check_gallery.py`, `check_doc_latency.py`
- [ ] 7.3 `npx -y @fission-ai/openspec@1.12.0 validate --all --strict`
- [ ] 7.4 `python3 tools/release_check.py --skip-slow`
- [ ] 7.5 The probe patch in `src/brush/stroke.cpp`, `src/mesh/sculpt.cpp`,
      `src/mesh/dynamic_sculpt.cpp`, `include/clay/mesh/sculpt.h` and
      `include/clay/mesh/dynamic_sculpt.h` is measurement scaffolding and is NOT
      in the PR; confirm `git diff origin/main` names no `probe_` symbol
