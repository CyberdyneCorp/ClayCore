# Tasks: a grey layer should not pay for colour

## 1. The skip

- [x] 1.1 A predicate over the absorbed set: two or more distinct
      `Node::color`, or any absorbed node whose `volume->has_color()`. Read
      from the same node list the bake already walks for `first->color`.
- [x] 1.2 `fill_colors_blocks` is called only when the predicate says the set
      can produce more than one colour. The resulting volume carries no colour
      channel otherwise.
- [x] 1.3 The node colour written on the result is unchanged, so a skipped
      pass still reports the layer's colour.
- [x] 1.4 The predicate is PUBLIC — `scene::layer_colors_vary` in
      `consolidate.h`. Not in the original plan: `test_consolidate.cpp`'s
      byte-identity reference reimplements the bake, so it has to apply the
      same rule, and a rule restated in a test is one that can drift from the
      code it checks. It is also a real part of what consolidation costs, so a
      host may want to ask before paying it.

## 2. Tests — the regression is the point

- [x] 2.1 A one-colour layer consolidates to a volume with `has_color()`
      false, and sampling it reports that colour. VERIFIED to fail against the
      unconditional pass: `CHECK_FALSE( baked->has_color() )` → `false`.
- [x] 2.2 A two-colour layer still consolidates to a coloured volume, both
      colours intact. This is the existing test and it did not move.
- [x] 2.3 A layer holding a coloured VOLUME, all node colours identical,
      consolidates with the pass taken and both sample colours surviving.
      VERIFIED to fail against a predicate written on node colours alone:
      `REQUIRE( baked->has_color() )` → `false`.
- [x] 2.4 Paint over a consolidated volume still overrides. Existing test,
      unchanged and passing.
- [x] 2.5 The byte-identity reference in `test_consolidate.cpp` fills colour
      through `scene::layer_colors_vary` rather than unconditionally. Found by
      the suite, not predicted: the reference and the bake had diverged on
      WHETHER a channel exists, which is a difference in the bytes.

## 3. The measurement, which is why this exists

- [x] 3.1 Mac, through the C ABI harness, medians of five at 1000 stamps:
      **278.2 ms** with the fix, against **371.5 ms** on main in the same
      session and **248.2 ms** at `196d403`, the last commit before colour.
      That is the `ac7460a` residual (271.9 ms) and nothing more, which is what
      this change set out to recover.
- [x] 3.2 Device gate on the reference iPad (`iPad15,5`, UDID
      `00008122-000410410A6B801C`), from a clean tree, thermal `nominal` at
      both ends. **`sdf_consolidate` 916.4 ms -> 678.8 ms**, back inside its
      786 ms budget and no longer a failure. It is still 1.29x the v0.30.0
      baseline of 524.3 ms — a larger residual than the Mac's 1.12x, so the
      out-parameter costs more on the tablet than it does here.

      **`mask_extrude` is NOT covered: 3695 ms -> 3787 ms, unmoved.** That was
      inference in the proposal and it was wrong; it needs its own bisect. It
      has also crossed its budget (3751 ms) rather than merely regressing.

      Recorded because a gate is not a scoreboard: the first attempt CRASHED,
      `testVoxelSessionsAndGallery` killed by signal on device, no results
      collected. The retry completed. Not reproduced, not diagnosed, and not
      counted as a pass for anything.
- [ ] 3.3 Commit `tests/device/last-gate.json` from the passing run. The run
      exists and passed; the stamp is not committable yet. Both halves below.

      **The gate passed.** Reference iPad (`iPad15,5`), clean tree, thermal
      `nominal` at both ends: 59 of 59 cases, no regression and no budget
      exceeded. `sdf_consolidate` **524.3 -> 397.9 ms**, below the v0.30.0
      baseline rather than merely back inside its 786 ms budget.
      `mask_extrude` 1.09x, `voxel_add_level` 1.03x, `sdf_stamp_cpu` 1.03x.
      `check_device_bench.py --update` was not run at any point, so the
      budgets in `baseline.json` are untouched.

      Both blockers this box used to name are accounted for. `mask_extrude`
      was the tape out-parameter, bisected in 3b and fixed on
      `perf/distance-only-tape-eval`. `sdf_stamp_cpu` was NOT device noise:
      the text here called it "very likely" noise, and `4e1b53e` recorded
      that as a wrong call — the scatter was real and the level under it was
      the same defect. Kept visible rather than deleted, because "looks like
      noise" going unwritten is why it stayed open.

      **The stamp is stale, so it is not committed.** That run was taken at
      `229976b`; main has since moved 77 commits. 42 engine-relevant paths
      changed and the ABI went 0.30.0 -> 0.37.0 including one deliberate
      break, so `release_check.py:check_device_gate` rejects a stamp naming
      `229976b` — correctly, and committing it would put a green stamp on an
      engine it never measured. `baseline.json` is byte-identical and still
      budgets the same 59 cases, so the experiment is the same shape; only
      the engine under it moved. Re-run `tools/run_device_bench.sh` against
      current main and commit THAT stamp. The parallel meshing/EDT/rasterize
      work, the CPU SIMD path and the gate-bake memo (#168) all landed since,
      so the numbers should come back at or better than the run above.

## 3b. Where mask_extrude's regression actually is

Bisected after 3.2 disproved the proposal's guess. Same harness method, the
device case's fixture, medians of three at 1000 stamps on the Mac:

| commit | | 1000 stamps | step |
|---|---|---|---|
| `50a301a5` | v0.30.0 | 3250.1 ms | — |
| `196d403` | before colour | 3281.3 ms | x1.01 |
| **`ac7460a`** | **the tape colour out-parameter** | **3751.7 ms** | **x1.14** |
| `85f1679` | the producers write colour | 3785.0 ms | x1.01 |
| `71118c1` | after the pass was pooled | 3806.7 ms | x1.01 |
| `b1868d4` | main | 3825.7 ms | x1.00 |
| this branch | with the colour-pass skip | 3843.1 ms | x1.00 |

- [x] 3b.1 It is `ac7460a`, not the colour pass. Every other step is inside
      noise, and this change's fix moves it by nothing — which is what the
      device already said and this explains.
- [x] 3b.2 The Mac accounts for x1.18 of the device's x1.51 across the whole
      range. Stated rather than papered over: the step is in the same place on
      both, and it is BIGGER on the tablet. `sdf_consolidate`'s leftover says
      the same thing — x1.12 residual here against x1.29 there — so the
      out-parameter costs more on device than it does on this machine, and the
      Mac cannot be used to size it.
- [ ] 3b.3 A change of its own. `ctape_eval` threads colour through every prim
      evaluation whether or not the caller wants colour, and its heaviest
      consumers — mask extrude, the bake's distance pass, meshing, raycast —
      want distance only. Not taken here: it is a kernel header compiled as
      five dialects, and bundling it behind a scene-module fix would put a
      cross-backend change in a PR nobody would review as one.

      **MEASURED, and the premise does not hold on CPU.** Removing the colour
      arithmetic from the combine path — the two `cmix` calls, leaving the
      distance arithmetic byte-identical — and re-running against an unpatched
      build, five repetitions each, medians:

      | | colour blend removed | baseline |
      |---|---|---|
      | `BM_MeshBricksGradGrownDoc` | 4.40 ms | 4.45 ms |
      | `BM_ConsolidateGrownDoc` | 83.6 ms | 83.0 ms |

      Inside noise, and the direction flips between the two benchmarks. A
      first reading looked like a 2.4x win on meshing (12.8 ms -> 5.31 ms) and
      was an artifact of `--benchmark_min_time=1x`: one cold iteration, which
      is not a measurement. Recorded because it is exactly the number somebody
      would quote to justify the change.

      **What this does NOT measure**, and what the premise would have to rest
      on instead: the experiment removed the colour *arithmetic*, not the
      colour *footprint*. `CTapeValue` is still 16 bytes and `CLAY_TAPE_MAX_STACK`
      is still 16, so the interpreter still moves 256 bytes of stack per
      evaluation either way. On a CPU that lives in L1 and costs nothing
      measurable, which is what the table above shows. On a GPU it is register
      pressure and therefore occupancy, which is a different quantity entirely
      and cannot be measured on this machine — lavapipe is a CPU implementation,
      so it would report the CPU's answer again.

      So the case for this change is a GPU case, and it needs a GPU
      measurement. Doing it for the CPU win would be doing it for a win that
      is not there.

## 4. What the gate could not see

- [x] 4.1 `BM_ConsolidateColoredGrownDoc` — the same 193-node layer with two
      colours in it — and a `FASTER_THAN` pair requiring the one-colour bake to
      beat it.

      NOT the millisecond floor the plan called for. A floor has to be tuned to
      whatever runner CI is on, and loose enough not to flake it would not have
      caught the 1.5x that shipped. The reason CI missed this was that
      `BM_ConsolidateGrownDoc`'s only gate compared it against
      `BM_ConsolidateSerialGrownDoc` and BOTH sides moved when the colour pass
      landed. A pair whose two halves differ only in whether the pass is taken
      cannot wash out that way, and needs no tuning.
