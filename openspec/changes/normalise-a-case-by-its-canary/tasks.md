# Tasks: divide a case by the machine it ran on

## 1. Establish that the engine did not change

- [x] 1.1 Reproduce `sdf_move` off-device through the C ABI, mirroring the
      device case exactly (1000 stamps, radius 0.4, displacement 0.05, rebuild
      between samples).
- [x] 1.2 A/B it against `libclaycore.a` from both endpoints of the window,
      INTERLEAVED over repeats — a first non-interleaved pass read
      0.85x / 1.93x / 1.08x, which is noise from a loaded machine and is the
      shape of a wrong answer here.
- [x] 1.3 Result: 1.00x / 1.04x / 1.05x, and the Mac's absolute numbers land on
      the device's good run. The engine is unchanged.
- [x] 1.4 Find what did change: relax and flatten went from 52 s and 53 s to
      6 s and 7 s when they were pooled, and the canary at `sdf_move` went from
      x1.07 to x1.50.
- [x] 1.5 Confirm the gap: `sdf_move` at 135 s, canary at 122 s and 147 s, so
      the attributed factor is interpolated across the transition itself.

## 2. Bracket every budgeted case

- [x] 2.1 `CaseResult` gains `canaryBeforeMs` / `canaryAfterMs`, documented as
      0 meaning "not bracketed" the way other absent context is.
- [x] 2.2 `RunCollector.sampleCanaryNow()` — takes a reading AND returns it, so
      a case can record the machine it was measured on.
- [x] 2.3 Bracket `measureAxis`, which is every verb case.
- [x] 2.4 Bracket the three hand-written latency cases, including both backends
      of `sdf_stamp_*` and `sdf_stroke_*`.
- [ ] 2.5 Gallery cases are NOT bracketed and fall back to raw. They set no
      per-case context at all, so bracketing them is a wider change to that
      file; recorded as a limitation rather than left to be discovered.

## 3. Normalise the comparison

- [x] 3.1 `bracket_factor` — mean of the two readings over the bundle's settled
      value, None when unbracketed.
- [x] 3.2 `normalised_p95` — returns the normalised figure AND the factor, so
      the raw number stays printable.
- [x] 3.3 Gate the normalised figure for both BUDGET and REGRESSION.
- [x] 3.4 Keep the 120 Hz frame-share note on the RAW figure: a frame share is
      about the time a hand waits.
- [x] 3.5 Derive budgets from normalised numbers, and keep `rawMs` /
      `machineFactor` beside them so a reader can see what was measured.
- [x] 3.6 Store the run's canary IN the baseline — without it the baseline's own
      cases cannot be normalised and the gate would compare normalised against
      raw.
- [x] 3.7 Note it loudly when one side is normalised and the other is not.

## 4. Tests

- [x] 4.1 The same engine at both ends of a thermal window agrees normalised and
      disagrees raw — the exact `sdf_move` case, as a test.
- [x] 4.2 A REAL regression still fails after normalising. Without this the
      change is a way to launder one.
- [x] 4.3 The bracket is the mean of both ends, not whichever is nearer.
- [x] 4.4 An unbracketed case is compared raw, not guessed at.
- [x] 4.5 A bracket with no bundle baseline is not normalised, following the
      rule `canary_factor` already sets.
- [x] 4.6 Existing records, which carry no brackets, produce byte-identical gate
      output.

## 5. Re-derive and validate on the device

- [x] 5.1 Run the suite on the reference iPad with brackets recorded. Valid,
      `nominal` at both ends, clean tree, 61 cases, 36 of them bracketed (the
      measure bundle) and 86 canary samples against the previous 15.
- [x] 5.2 Re-derive the baseline from that run.
- [x] 5.3 `sdf_move` normalises to **0.0791 ms** against the cold run's
      **0.079 ms** — 1.00x. And it passes because it is normalised, not because
      the budget grew: the budget got TIGHTER, 0.1279 -> 0.1187 ms. So did
      `sdf_relax` (2.53 -> 0.82) and `sdf_consolidate` (991.5 -> 373.8, which
      had been budgeted against a 661 ms measurement that stopped being true
      several changes ago).
- [x] 5.4 The bracket's cost is not material, and its VALUE showed up
      immediately: `sdf_move`'s two readings are 159.0 ms before and 135.6 ms
      after — the case starts hot behind `sdf_flatten` and cools during itself.
      Either end alone is wrong in a different direction, which is the case for
      the mean and against "whichever sample is nearer".

## 6. Keep the report and the gate telling the same story

- [x] 6.1 Caught while validating 5.2: drift attribution still used the nearest
      periodic sample while the gate divided by the bracket. They disagree —
      `sdf_flatten` reads x1.21 nearest and x1.43 bracketed — so the run printed
      "every case was measured with the canary inside tolerance" while applying
      a 43% correction to it. Attribution now follows the bracket.
- [x] 6.2 Tests for both directions: attribution follows the bracket when there
      is one, and still falls back to the nearest sample when there is not.
