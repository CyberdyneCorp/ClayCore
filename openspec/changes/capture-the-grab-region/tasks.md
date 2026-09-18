## 1. Measured before building

- [x] 1.1 Issue #620 reproduced on origin/main at `2880560c`, ABI 0.120.0:
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

- [x] 2.0 **MEASURED BEFORE BUILDING THE REST, AND IT CHANGED THE DESIGN.** 3.1
      was built on its own, behind a probe switch, on twelve fixtures (the six
      of 1.5b with the drag's sign flipped on each). Full numbers in
      `proposal.md` §8–§11; the verdict: **the maintenance keeps A high-weight
      entry alive but not THE one, and D2 as written is not sufficient.** The
      splits DO repopulate the core — 9 captured entries become 59–64 live, 45
      become 434–538 — and the top surviving weight floors at **0.710** against
      unmaintained B's **0.000**. But the weight-1 centre is collapsed in the
      first half of the gesture and a split's MEAN can never recreate it, so
      D2's rules alone reach **72.98–97.10%** and leave the two representations
      disagreeing by **1.7e-2 – 3.7e-1** — worse than today's A (2.9e-4 – 3.6e-3)
      and outside 4.1's 1e-3. The named alternative closes it: with a collapse
      forbidden to retire a carried vertex, reach is **99.99–100.03% on all
      twelve**, the top surviving weight **1.000 on all twelve**, and the
      agreement **0.0 – 1.6e-4**. The feared cost did not appear — the surface it
      leaves is FINER than the unprotected maintenance (max edge 0.2689 against
      0.6215 on the 1.5 push-in, and equal to today's A on four of six fixtures)
      and the stroke runs at **0.39x–0.89x of A**. Two rules D2 did not have were
      found by the counters and are now in it: a split with ONE carried parent
      inserts too, and a carried vertex the REMESHER moved takes the same shift
      in its captured position (1–291 of them per stamp, every stamp; without it
      the relaxation inside a Grab does nothing). `design.md` D2 is revised to
      five parts and D3 is no longer the fallback. Probe validity: preconditions
      asserted per fixture and green on all twelve (something captured, 220–1357
      splits, one row per stamp, the surface validates, M both inserted and
      retired, the protected arms refused a collapse and retired none, the arms
      are not byte-identical, mode 0 still matches the unpatched fixed path);
      with the switch unset the full suite is **2839 of 2839 cases and
      17,931,111 of 17,931,111 assertions**, so the scaffolding is inert

- [x] 2.1 `MeshSculptor::begin_carried_region` / `end_carried_region` /
      `carrying`, with `gather` returning early while one is open and taken. The
      header states the write and why it carries no accumulation error, and says
      that a FIXED mesh has no mitigation for a region a long drag stretched and
      cannot have one
- [x] 2.2 `mesh_stamp_settings` gives Grab `direction = s.position - first` and
      `center = first`, in the one place all three consumers share. The remesh
      centre following the stamp is applied in `apply_to_dynamic` alone, because
      the other two have no remesher — stated there rather than implied
- [x] 2.3 `CarriedRegionScope` opens the capture for `grab` and closes it on
      the way out of all three consumers, so no early return can leave a sculptor
      carrying. The capture is TAKEN lazily, inside the first gather that reaches
      something, which is what makes "a stroke that applies nothing captures
      nothing" true with no special case
- [x] 2.4 `apply_to_multires` takes the same `mesh_stamp_settings`. A rebind
      DOES need code: `MultiresSculptor` owns the capture while it is open and
      `bind` re-opens it on the sculptor it just built, moving
      `capture_generation` so the consumer re-anchors the drag it measures from.
      Without that the stamp after a rebind writes the whole drag onto a region
      that has already taken most of it. 4.6 is the case
- [x] 2.5 Measured with the cognitive-complexity skill; see §7.6

## 3. Engine — the adaptive surface

- [x] 3.1 `DynamicSculptor::carry_on_split` / `carry_on_collapse`, over the
      `carry_items_` / `carry_captured_` / `carry_weights_` arrays and a
      generation-checked slot map
- [x] 3.1a `carry_may_collapse`, consulted before the operator runs. Reverted,
      3 cases and 26 assertions fail
- [x] 3.1b The one-parent branch of `carry_on_split`, counted separately as
      `inserted_one_parent`. Reverted, 2 cases and 4 assertions fail
- [x] 3.1c `carry_on_move`, fed by both the collapse survivor and
      `relax_region`'s writes. Asserted through a trailing zero-drag stamp that
      must change nothing; reverted, that case fails
- [x] 3.1d Shipped as `mesh::RemeshHooks` in `remesh_local.h`: a borrowed
      context and four optional function pointers, defaulted to null on both
      `remesh_region` and `relax_region`. No caller in `src/`, `bindings/` or
      `tests/` passes one except the sculptor maintaining its own region. No
      RemeshObserver, no probe_ symbol and no env switch is in the diff
- [x] 3.2 Done, and **its justification is refuted while the rule stands**. A
      remesh left at the anchor does NOT leave a coarser surface once the region
      is maintained: same surface-wide longest edge (0.1450 against 0.1466) and
      40% MORE work (1381 splits against 980). What it costs is the TIP — longest
      edge there 0.1450 against 0.0487, 25 vertices against 65. `stroke.h` and
      `clay.h` carry the replacement, `design.md` D2 (2) is corrected, and
      `proposal.md` §16 has the table
- [x] 3.3 `DynamicSculptor::CarriedRegionCounters`: `captured`, `inserted`,
      `inserted_one_parent`, `retired`, `moved`, `collapses_refused`, plus
      `carried` and `top_weight` snapshotted as the capture closes so a caller can
      read them after the stroke returns. `collapses_refused` marks a carried
      entry the first time a collapse is refused on it and counts it ONCE for the
      gesture, so the region bounds it and the case asserts that bound —
      deduplicating per STAMP was tried first and read 616 against 891 raw
      events, which is not an order of magnitude and is bounded by nothing
- [x] 3.4 Asserted on every adaptive fixture in the reach and maintenance
      cases

## 4. Tests

- [x] 4.1 (threshold derived; the case still to write) Reach: a 0.6 pull-out Grab
      reaches the whole drag on the fixed mesh and on the adaptive surface, and
      the two agree to 1e-3 — the #619 constraint, re-asserted rather than
      dropped. **Re-derived under 2.0 for the rule that will actually ship**
      rather than for candidate Q: over twelve fixtures the maintained,
      protected rule agrees to **0.0 – 1.6e-4**, so 1e-3 has 6x headroom and is
      the right threshold. Assert reach >= 0.99 and not == 1.0: the rows read
      99.99–100.03%, and the 100.03 is a real overshoot — D2 (5) lets the
      weight-1 vertex keep the tangential slide the relaxation gave it
- [x] 4.2 Eight fixtures in one case: both poles, both signs of the drag, 0.6
      and 1.5, at radius 0.15 / detail 4 where an unmaintained region dies and at
      radius 0.30 / detail 8 as the control. Measured agreement 9.1e-5 – 1e-3.
      NOTE the sign flip is of the DRAG and not of the anchor: starting a stroke
      0.6 off the surface reaches nothing and captures nothing, which the first
      spelling of these fixtures did
- [x] 4.3 Both. The curve asserts the CHORD and asserts that it is not the path
      (the fixed path reaches 100.0% of the chord and under 98% of the path). And
      it found something: **the ADAPTIVE curve reaches 115.8% of the chord**,
      because a child adopted from one carried parent is born on material the arc
      swept and then takes its own share of the remaining drag. It is the
      maintenance and not the rule — with the remesher off the same fixture agrees
      to 1e-3, asserted in the case — and it is not a regression (re-gathering
      disagrees by 1.3e-3 there). Bounded by the case, recorded in the release
      notes and in `proposal.md` §17
- [x] 4.4 Asserted on three fixtures with `captured > 0`, `splits > 0` and
      `collapses > 0` as preconditions: entries == captured + inserted +
      inserted_one_parent − retired, entries > captured, both split rules fired,
      **retired == 0**, and refusals bounded by the region. Measured 9 → 195 and
      45 → 811
- [x] 4.4a `carried_top_weight`, snapshotted into the counters as the capture
      closes, asserted at 1.000 on every fixture
- [x] 4.4b Asserted against the no-remesher control measured IN the case
      (`topology.enabled = false`, with `splits == 0` asserted so the control is
      really a control), not against a remembered constant: the protected rule
      leaves under half the control's longest edge and under 0.4 absolute
- [x] 4.5 **Eight mutations, each removed from the shipped source, rebuilt, and
      the build success AND a changed binary asserted before the result was read.**
      That check earned itself: the first harness ran `git checkout` over an
      uncommitted edit, every build after the first failed, the stale binary was
      re-run and all eight mutations reported identically. Committed first, then
      re-run:

      | mutation | cases failing | assertions failing |
      |---|---|---|
      | rule 3, a collapse may retire a carried vertex | 3 | 26 |
      | rule 5, the remesher's move is not followed | 1 | 1 |
      | rule 4, a one-parent split is dropped | 2 | 4 |
      | rule 1, the adaptive region is re-gathered | 4 | 22 |
      | rule 2, the remesh centre stays at the anchor | **0**, then 1 | **0**, then 3 |
      | the maintenance, splits not published | 4 | 28 |
      | rule 1, the fixed region is re-gathered | 4 | 20 |
      | the deformation rule itself | 4 | 24 |

      **The rule-2 row is the finding and it is why this task exists.** The case
      as first written asserted the WHOLE SURFACE's longest edge and passed with
      the rule reverted, because that maximum lives in the neck behind the tip
      either way (0.1466 following, 0.1450 anchored). Rewritten against the tip —
      0.0487 against 0.1450, and 65 vertices within a brush radius against 25 — it
      fails 3 assertions when reverted. See `proposal.md` §16
- [x] 4.6 "a multiresolution grab reaches the whole drag" — the first Grab
      through `apply_to_multires` in the tree. Reverting the fixed path's capture
      fails it, which is the evidence it reaches the shared rule

## 5. The goldens that move, and what each becomes

Measured by running the suite under each candidate rule, not predicted. Under B
exactly two cases fail, both in one file; under C a third does.

- [x] 5.1 `hand_loop` updated to the carried rule: it opens a carried region for
      Grab, drags by the motion since the first sample, and follows the remesh
      centre once the region is captured. Predicted 6 assertions at :224–232;
      measured exactly those six
- [x] 5.2 `recorded_loop` updated the same way. Predicted 2 assertions at :778
      and :779; measured exactly those two
- [x] 5.3 Kept, unchanged and passing, with the how-far assertion beside it:
      the grab's travel along the drag equals the drag to within 2%
- [x] 5.4 Verified rather than trusted, twice: `run_case` sets `s.center` per
      step and calls `MeshSculptor::stamp`, so no stroke consumer and no capture
      reaches it; and `git diff origin/main --stat` names none of the three
      `.inc` files. The parity case passes unchanged
- [x] 5.5 Confirmed on the shipped code: the only two cases that moved are 5.1
      and 5.2, and the full suite is green everywhere else
- [x] 5.6 Confirmed on the FULL suite and not only the targeted shard: under the
      captured-set rule it is **2837 of 2839 cases, 8 of 17,931,111 assertions
      failing**, and they are exactly 5.1 and 5.2; under a following centre
      **2836 of 2839, 9 assertions**, adding exactly 5.3. Nothing else in the
      tree moves under either rule. Re-run independently, with the captured-set
      rule made the DEFAULT in all three consumers rather than switched on:
      2839/2839 and 17,931,111 assertions at origin/main, then 2837/2839 and
      17,931,103, the eight failures being `test_dynamic_stroke.cpp:224, 228,
      229, 230, 231, 232, 778, 779` and nothing else. 5.4 re-checked by reading:
      `run_case` sets `s.center` per step and calls `MeshSculptor::stamp`, so no
      stroke consumer and no capture reaches it
- [x] 5.7 pyclay built and its pytest run through `release_check.py`, which
      configures `CLAY_BUILD_PYTHON=ON` and passes `--require-import` so the
      binding-parity gate compares against a BUILT module rather than against the
      source. See §7.4. The Swift surface and the device suite are NOT run: no
      mesh brush is in the device suite and no Swift signature moved — stated
      rather than implied

## 6. Documentation and spec

- [x] 6.1 All three, plus the curve's chord, plus the statement that a FIXED
      mesh has no mitigation for a stretched captured region and cannot have one
      (longest edge 0.82 after a 1.5 pull from a starting 0.09)
- [x] 6.2 Both, at :8447 and :8723, with the chord and an explicit note that
      the behaviour changed without an ABI change
- [x] 6.3 The Grab sentence, the "Not provided" note, and the "known question
      left open" paragraph replaced by "A grab carries the region it captured
      (#620)" with the five rules and what each was measured to be worth —
      including the correction to the remesh-centre row
- [x] 6.4 Stated in `docs/07` §8c beside the new section and in `docs/05`
      beside `layer.move_surface`, both naming it a field-inversion problem rather
      than an anchoring one, and both saying it is not fixed here
- [x] 6.5 `CLAY_ABI_*` and `kSceneMinor` unmoved, verified in the diff; no
      pyclay or Swift signature moves. Both pyclay docstrings updated, and a third
      beside them (`apply_stroke(record=)`'s "keeping the stroke's Grab and
      Snakehook centres"), which the list omitted
- [x] 6.6 `docs/release-notes/v0.120.0.md`, following the per-version convention
      (`v0.113.0.md` and `v0.116.0.md` were likewise written in the PR that made
      the change, not at release time). It carries the two mandatory sections and
      the per-preset TABLE, because the shortfall depended on the brush radius
      (22.66% at r 0.15, 41.10% at 0.30, 60.56% at 0.50 on the same 0.6 drag) and
      on the drag (41% at 0.6, 18% at 1.5): a host feel travels 1.65x to 10.8x
      further depending on its preset, so there is no single factor to divide by

## 7. Verification

- [x] 7.1 Full unit suite **2847 of 2847 cases, 17,931,271 of 17,931,271
      assertions**, from 2839 / 17,931,111 on origin/main — the eight new cases
      and 160 new assertions, with the two that moved passing under the updated
      rule
- [x] 7.2 Run through `release_check.py`; see §7.4 for the table and for the two
      rows that fail on this machine for reasons that predate the branch
- [x] 7.3 `validate --all --strict` at the pinned 1.12.0
- [x] 7.4 `release_check.py --skip-slow`; see §7.7
- [x] 7.6 **Cognitive complexity, measured — and the first measurement was
      invalid.** clang-tidy `readability-function-cognitive-complexity` run with
      `-p build/release` found NO COMPILATION DATABASE, so every file was parsed
      with its primary header missing and the scores it printed were of a
      degraded AST: it reported `remesh_region` at 15 both with the hooks and
      without them, which is what sent me looking. Re-run against a real
      `compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`), with
      `clang-diagnostic-error` counted and zero on both trees:

      | function | origin/main | first spelling | shipped | target |
      |---|---|---|---|---|
      | `remesh_region` | 132 | **170** | **136** | 15 |
      | `relax_region` | 39 | 42 | **38** | 15 |
      | `apply_to_mesh` | 19 | 19 | 19 | 15 |
      | `apply_to_multires` | 21 | 24 | 24 | 15 |
      | `apply_to_dynamic` | 12 | 15 | 15 | 15 |
      | `DynamicSculptor::stamp_impl` | 24 | 26 | 26 | 15 |
      | `DynamicSculptor::gather` | 11 | 14 | 14 | 15 |
      | `MeshSculptor::gather` | 9 | 12 | 12 | 15 |

      The four null tests per operator added **38** to `remesh_region`, which is
      the highest score in that file before anything is added to it. They are now
      five one-line helpers in its anonymous namespace (`split_ends`,
      `notify_split`, `may_collapse`, `position_if_watched`, `notify_collapse`,
      `commit_relaxed_positions`), which takes the addition to **+4** and leaves
      `relax_region` one BELOW where it started. Every function this change
      ADDS is inside the backend target: `carry_on_split` 8,
      `rebuild_from_carry` 9, `carry_index_of` 5, `commit_relaxed_positions` 4,
      `notify_collapse` 3, `carried_live` / `carried_top_weight` /
      `end_carried_region` 3, `split_ends` / `notify_split` / `may_collapse` /
      `position_if_watched` / `carry_may_collapse` / `carry_on_collapse` 2.
      `remesh_region`, `relax_region`, `apply_to_mesh`, `apply_to_multires` and
      `stamp_impl` are over the target and were over it on origin/main; stated
      rather than mangled, as the rule allows
- [x] 7.7 `release_check.py --skip-slow`: **PASS** on version (0.120.0 all three
      files, unmoved), configure, build, tests (11 of 11 ctest entries including
      the pyclay pytest), parity (48 cases, 1,411,877 assertions, `compared cpu`
      — which says nothing about cuda/opencl/vulkan and the row says so),
      layering, licenses, bindings (`imported .../pyclay.cpython-311-darwin.so`,
      a BUILT module and not the source-against-itself fallback), kernels, abi
      and openspec. **FAIL** on `dialect` (no Metal Toolchain on this machine),
      `device` (the iPad gate ran at 1bc981aba and the engine has moved since)
      and the four `hardware/*` rows (all four say the same thing: they ran at
      0c3a392cb and `include/clay/kernel/tape.h` changed after, which #618 did,
      not this branch). All six fail identically on origin/main.
      **`task-symbols` failed and it WAS this change's**, 9 unresolved names —
      including three that predate this stage on this branch. Backticks around
      origin/main, RemeshObserver, probe_ and build/ make the checker look for
      files and symbols that do not exist; unquoted, it reads
      `task symbols resolve in 62 change(s), 5 baselined`
- [x] 7.8 The two REFUTATIONS this stage produced are in `proposal.md` §16 and
      §17 and are reflected back into `design.md` D2 (2) and its open questions,
      `docs/07` and `include/clay/brush/stroke.h`, rather than left in the
      proposal only: the remesh centre does not buy the surface-wide edge it was
      justified on, and a curved drag on the adaptive surface reaches past the
      chord

- [x] 7.5 `git diff origin/main` names no probe_ symbol, no RemeshObserver
      and no `getenv`. The shipped publication interface is `mesh::RemeshHooks`
      and the shipped carry is `DynamicSculptor`'s own, neither behind a switch.
      build/ is gitignored and the probe drivers stay there
