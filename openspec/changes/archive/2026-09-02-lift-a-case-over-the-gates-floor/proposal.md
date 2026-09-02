# Lift a case over the gate's floor

## Why

`check_device_coverage.py` reported `coverage: OK — 60 verb(s) measured`. For
**39 of the 61** entries, "measured" meant a number was recorded — not that a
regression in that verb would ever be caught.

`check_device_bench.py` requires a regression to be **both** relatively large
(`> 1.4x`) **and** absolutely meaningful (`> NOISE_FLOOR_MS`, 0.05 ms). The
floor is right — a 20 µs difference on a tablet is not a difference — but it
means a case can only fail above **0.125 ms**. Below that the row is
decorative.

`fix/coverage-says-gated-or-reported` (#338) made the report say so, which was
the honest half of issue #337 and changed no verdict. This is the other half:
the cases themselves.

The worst were not marginal. `trim_curve` measured 0.0001 ms and would have
needed a **401x** regression before the suite noticed; `cut_create` 300x;
`armature_edit` 101x. Twenty-one of the thirty-nine were `devicemeasure` cases,
so it was never a gallery problem.

## What changes

**A timed body performs `batch` applications of its verb, and the record says
how many.** `Measurement` gains a `batch` field, defaulting to 1, so every
figure stays a statement about the verb rather than an unexplained
hundredfold jump — a per-application cost is `p95Ms / batch`.

Batching rather than a bigger per-application workload, because for most of
these no size knob exists that keeps the verb the same thing: `cut_create`
resolves a fixed rect, and reaching 300x through its only size input would mean
a ~3,000-vertex lasso, which is not the gesture the verb names. Where a knob
does exist the repo already uses it — `voxel_smooth_r32` is exactly that, and
stays.

- **The verb cases** (`devicemeasure`) take a `batch:` argument, 128 for the
  voxel brushes up to 4096 for `trim_curve`.
- **The voxel session cases and the mask freeze** (`devicegallery`) time one
  DRAG of 64 dabs rather than one dab of it — which is what those cases already
  claimed to measure. `mask_freeze` moves from `interactive` to `gesture` with
  it, because the timed unit is now a drag.
- **The three SDF session cases** deposit several items per stroke instead of
  one, sized per case so the growth exponent stays well under the gate's 1.25.
- **`move_drags`** is delivered in four sub-steps of a quarter of the
  displacement, which is what a real pointer drag does. Four and no more: see
  below.
- **`sdf_stamp_bricks`** is NOT batched — its reset removes the previous node,
  so batching would let stamps 2..K append onto an un-invalidated document,
  which is the suffix-compile path `sdf_stroke_bricks` owns. Its axis gains a
  10,000-stamp point instead, which is its own documented instrument.
- **`sdf_stroke_bricks`** stops dividing its whole-stroke figure by 24 and
  records the stroke with `batch: 24`.

## What this does not change

No verdict, no tolerance, no floor. `NOISE_FLOOR_MS` stays 0.05 and the 1.4x
tolerance stays — both are right, and lowering either would buy sensitivity by
spending the false-failure budget that #331, #333 and #336 were spent earning.
What changes is the scale the cases are measured at.

## Impact

- Affected specs: `device-gate`
- `tests/device/baseline.json` is re-derived for every changed case.
- 22 GATED / 39 REPORTED ONLY becomes 61 GATED / 0.
