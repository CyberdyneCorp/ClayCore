# Tasks: measure the Lipschitz where the samples are

## 1. The traversal

- [x] 1.1 `FieldVolume::measure_sample_lipschitz()` walks `index_`, skips
      `entry < 0`, and reads `data_` directly. Three strided sweeps per brick —
      `1`, `kBrickDim + 1`, `(kBrickDim + 1)^2` — each stopping one sample
      short along its own axis so the forward neighbour is always inside the
      block. No `sample_at`, no `std::optional`, no coordinate division.
- [x] 1.2 The halo sample is included: the per-axis loops run to
      `kBrickDim + 1` on the two axes they do not step along, and to
      `kBrickDim` on the one they do. Dropping it would drop exactly the pairs
      that straddle a brick boundary.
- [x] 1.3 The clamp is unchanged — `max(1.0f, steepest / cell_size_)` — so a
      field that is genuinely 1-Lipschitz still measures 1 and nothing that
      was honest before pays for the change.
- [x] 1.4 The comment carries the equivalence argument, not just the claim: a
      forward pair lies wholly inside brick `g / 8`, and if that brick is
      unstored then so is the pair's upper end. A reader who does not believe
      it can check it.

## 2. Tests — the regression is the point

- [x] 2.1 `measure_sample_lipschitz matches the dense traversal it replaced`
      keeps the OLD traversal in the test as an independent oracle and requires
      `==`, not `Approx`. Both take a max over differences between the same
      stored floats, so a tolerance would only hide a dropped pair.
- [x] 2.2 The oracle runs against a field with structure at the SAMPLE scale,
      for the reason `sample_parallel`'s parity test uses one: a smooth
      analytic field looks plausible whichever pairs a traversal missed. Three
      resolutions, plus two steep fields, because a volume that measures 1
      tells nothing apart.
- [x] 2.3 `measure_sample_lipschitz compares a brick's halo sample` plants the
      steepest pair at locals 7 and 8 of the first brick and the second
      steepest one sample further along, at locals 0 and 1 of the next. A sweep
      confined to the samples a brick owns reports 1.5 instead of 2.5 — a
      changed value, not a shaded one. VERIFIED to fail against a loop bounded
      at `kBrickDim`.
- [x] 2.4 The field is zero everywhere before the pattern is planted, so every
      brick is stored and the measurement is the planted pattern rather than an
      accident of which bricks the band kept.
- [x] 2.5 Full unit suite: 1413 cases, 3,716,226 assertions, no failures. The
      existing declared-bound assertions in `test_consolidate.cpp` are
      untouched and still pass — the value they check is the same value.

## 3. Benchmarks

- [x] 3.1 `BM_ConsolidateGrownDoc` 54.0 → 47.4 ms.
- [x] 3.2 `BM_ConsolidateSerialGrownDoc` 407 → 404 ms, and the gate that
      requires the batched bake to beat the serial one still holds. The serial
      side barely moves because it is dominated by point-at-a-time tape
      evaluation, which is #271.
- [ ] 3.3 No new benchmark. The bake is already gated by the pair above, and a
      microbenchmark of the measurement alone would gate a function that
      #272's work may fold into a dirty-brick pass.

## 4. The device gate

- [x] 4.1 Full 59-case run on the reference iPad (iPad15,5, iPadOS 26.5.2)
      from a clean tree at `78846a7`. `valid: true`, `treeDirty: false`,
      thermals nominal at both ends. Compared against the run of 2026-08-24 on
      `66f7a45`, whose `tests/device/` measurement code is byte-identical to
      this one — checked, not assumed: `git diff 66f7a45..HEAD -- tests/device`
      touches only `last-gate.json`.
- [x] 4.2 p95 at 1000 stamps, except `volume_hpolish`, whose axis is passes:

      | case | before | after | |
      |---|---:|---:|---|
      | `sdf_flatten` | 6.677 | 3.153 | **2.12x** |
      | `volume_hpolish` (4) | 149.673 | 72.170 | **2.07x** |
      | `sdf_consolidate` | 340.462 | 314.686 | 1.08x |
      | `mask_extrude` | 4442.964 | 4286.490 | 1.04x |
      | `sdf_relax` | 1.661 | 1.669 | flat |
      | `sdf_stamp_cpu` | 2.795 | 2.474 | flat |
      | `sdf_stamp_bricks` | 1.700 | 1.690 | flat |
      | `sdf_stamp_metal` | 2.040 | 2.033 | flat |

- [x] 4.3 `sdf_relax` is the CONTROL and it is the reason to believe the rest.
      Relax rewrites samples without re-measuring the bound — it is the one
      verb in the group that never calls `measure_sample_lipschitz()` — so it
      had nothing to save, and it saved nothing. Every case that does call it
      moved; every case that does not stayed flat. The size of each move
      tracks how much of that verb's work the measurement was: flatten's own
      arithmetic is cheap and the measurement dominated it, consolidate's bake
      is dominated by tape evaluation instead.
- [x] 4.4 `tools/check_device_bench.py` against the committed baseline: OK,
      no case over budget. The canary drifted x1.53 across the run and five
      GALLERY cases at the end of their bundle were measured past x1.3 of its
      settled value; their budgets still hold, and none of the cases above is
      one of them. `last-gate.json` was restored rather than committed — a
      release stamp is not this change's to move.

## 5. Documentation

- [x] 5.1 `docs/09-brush-latency-and-coverage.md`: five rows re-derived from
      the run, and the `‡` note rewritten from "known stale, in the safe
      direction" to what was actually measured, including the control.
- [x] 5.2 `docs/05-claycore-library.md` §9 item 4, which said "re-measure on
      device before relaxing either". It has now been re-measured, so it says
      the number instead.
- [x] 5.3 Both notes say plainly that `sdf_relax` and `mask_extrude` read
      WORSE than the table used to claim, and that this is drift rather than a
      regression: the table had fallen out of step with
      `tests/device/baseline.json` in both directions. Filed as #273 rather
      than fixed by hand inside this change — the rest of the table has not
      been re-derived, and saying so is the point.

## 6. Not in this change

- [ ] 6.1 The serial C ABI bake path (#271) and the whole-band relax
      traversal (#272), in that order. The profile should be re-taken after
      all three rather than predicted now — this change already moved
      `sdf_flatten` by 2.12x, which is enough to invalidate a prediction made
      before it.
