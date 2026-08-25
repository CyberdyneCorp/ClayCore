# Divide a case by the machine it ran on

## Why

`make-device-timing-position-independent` made the conditions visible and said
what it was leaving for later: *"deciding what to do about a drifting run is the
next change, and needs this one's data first."* This is that change, and the
data arrived as a false failure.

On 2026-08-25 the gate reported:

```
sdf_move: REGRESSION 0.137 ms p95 vs baseline 0.085 ms (x1.60, tolerance x1.4)
```

**The engine had not changed.** `clay_layer_move_surface` is CPU-only, so it can
be measured off-device: a probe through the C ABI mirroring the device case,
linked against `libclaycore.a` from both endpoints and interleaved A/B/A/B over
five repeats of sixty samples, reads **1.00x / 1.04x / 1.05x** across the growth
axis. The Mac's absolute numbers also land on top of the device's *good* run.

What changed was the thermal window in front of the case. `mask_extrude` runs
first and takes ~116 s in both runs, and the canary reads x1.07 right after it in
both. Then:

| | before | after |
|---|---|---|
| `sdf_relax` | took **52 s** | took **6 s** |
| `sdf_flatten` | took **53 s** | took **7 s** |
| `sdf_move` ran at | 225 s | 135 s |
| canary at `sdf_move` | **x1.07** | **x1.50** |

The perf program pooled relax and flatten across every core. **105 seconds of
single-threaded work left the device at x1.07; 13 seconds of pooled work takes
it to x1.50.** The cold window `8d13b47` established has been consumed, and
re-ordering cannot restore it — the heat sources are themselves budgeted cases,
so they cannot all run first.

## The part that makes it a gate bug rather than a scheduling problem

`sdf_move` runs at 135 s. Its nearest canary samples are at **122 s (x1.07)** and
**147 s (x1.50)** — a 25-second gap containing both pooled cases. The factor the
gate attributes is interpolated across the very transition that matters, so
**the verdict depends on where a 30-second timer happened to fire.** The earlier
run looks clean only because its timer landed at exactly 225 s.

A gate whose answer depends on timer phase is not measuring the engine.

## What changes

- **Every budgeted case is bracketed.** The canary is sampled immediately before
  and immediately after each case and both readings are recorded on it. Nothing
  is interpolated. The canary is ~130 ms, so two per case is ~16 s over a run
  that already takes ~250 s.
- **The comparison is normalised.** A case's measurement is divided by how slow
  the machine was while it ran, and it is the normalised figure that is gated —
  against a baseline derived the same way.
- **The raw figure stays visible.** It is printed beside the normalised one, and
  the 120 Hz frame-share note deliberately uses it: a frame share is about the
  time a hand waits, and a throttled device still makes it wait.

## Non-goals

- **No engine work.** Nothing here makes the device faster or cooler. That the
  pooled verbs now heat an iPad this fast is a real finding about the perf
  program and is tracked separately.
- **No re-ordering.** Establishing a cold window was `8d13b47`'s approach; this
  change makes ordering stop mattering instead of chasing it again.
- **The gallery bundle is not bracketed.** Its cases set no per-case context at
  all, so they fall back to a raw comparison — exactly what they get today.

## Capabilities

### Modified Capabilities
- `device-gate`: the canary stops being only a verdict about the run and becomes
  the denominator a case is judged against.

## Impact

- `tests/device/Shared/LatencyHarness.swift` — the two fields, and a sampler
  that hands its reading back.
- `tests/device/Measure/VerbCases.swift`, `tests/device/Measure/LatencyCases.swift`
  — every budgeted case bracketed.
- `tools/check_device_bench.py` — `bracket_factor`, `normalised_p95`, and the
  budget derivation.
- `tests/device/baseline.json` — **re-derived once**, because the numbers now
  mean something different.
- `bindings/python/tests/test_device_canary_attribution.py` — including that a
  real regression still fails after normalising, which is the property that
  stops this becoming a way to launder one.
