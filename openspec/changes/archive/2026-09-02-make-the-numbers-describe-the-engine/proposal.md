# Make the numbers describe the engine

## Why

Three published numbers had stopped describing what the engine does, in three
different files, and none of them could say so.

**1. Three budgets could not fail.** `volume_hpolish` measured 3.37 ms against a
132.40 ms budget — it could have got **36x slower** with nothing objecting.
`volume_flatten` 26x, `sdf_flatten` 17x. The regression arm was no help either:
`write_baseline` writes `budgets` and `cases[]` from the same run, so both went
stale together and the regression reference tripped at 123.57 ms.

This is issue #337 from the other side. There a case recorded a number it could
never object to because the MEASUREMENT was under the gate's floor; here the
BUDGET is over the measurement. The cause is the same — a number that stopped
tracking the engine — and so is the consequence.

**2. The coverage check overstated what it protects.** `gate_reach` computed
its ratio from the RAW worst p95 while the gate decides on the NORMALISED one.
The two part company exactly when the device was slow while a case ran, and
`sdf_move` was that case: raw 0.1324 ms reads as gated, normalised 0.1158 ms
needs 1.43x. The tool called protected a verb the gate cannot fail.

**3. `docs/09`'s table and the baseline disagreed, in both directions** (#273).
`pose_region` read 7.6x optimistic and `magnify_pinch` 2.9x; a dozen voxel rows
read 2-3x pessimistic. The table is what a host reads to size progress UI, and
an optimistic row under-budgets the work.

## What changes

- **`BUDGET_SLACK`**: the gate reports a budget sitting over 6x its case's
  measurement, with the class printed. Reported, never failed — a generous
  ceiling can be deliberate over content-varying work, and every case that has
  tripped it so far is `operation` class.
- **`gate_reach` judges the normalised figure**, matching what the gate decides
  on.
- **The three budgets are re-derived**, from the top of each case's band across
  valid bracketed runs rather than 1.5x one draw — see the design.
- **`tools/check_doc_latency.py`** fails CI when a quoted figure and the
  baseline disagree, per bundle. The table is regenerated from the baseline.

## What does NOT change

`sdf_move`'s baseline. It measures 1.46x its committed figure and that is a
real regression, bisected to v0.52.2 (#358). Re-deriving would enshrine it: the
budget is the only ABSOLUTE memory the gate has, because the regression
reference resets on every re-baseline, so a 1.35x slip per release would pass
forever and accumulate.

Neither the tolerance nor the noise floor moves. Both bound FALSE failures.

## Impact

- Affected specs: `device-gate`
- Closes #273. Records #358 rather than hiding it.
