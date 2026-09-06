# Proposal: a pad that moves on every dab empties the brick cache

## Why

A brick's seed is only reusable if it was computed under the same cull pad:

```cpp
// bindings/c/clay_c.cpp:1501
if (e->pad != pad) return s;   // "The pad only grows on an append, so this is
                               //  a real gate rather than a formality."
```

Exact float equality. And the pad is not constant — `chain_pad_envelope`
(`src/scene/bounds.cpp:777`) is:

```cpp
if (nodes <= 75) return fit.base;
return fit.base + fit.slope * std::log2(float(nodes) / 75.0f);
```

with `profile_chain_pad` clamping it at the profile's support. So for quadratic
(base 2.80, slope 0.35, support 4k) the pad is:

| total nodes `n` | pad | seeds |
|---|---|---|
| `n <= 75` | constant `2.80k` | survive an append |
| `76 <= n <= 807` | `k * (2.80 + 0.35 log2(n/75))` — **changes on every node added** | every append invalidates every one |
| `n >= 808` | clamped at `4k` | survive an append |

In the middle band the brick resume is not degraded, it is **dead**: every dab
changes the pad, so every seed fails the equality gate and every brick takes the
full walk. Everything `resume-the-brick-refill` and `seed-a-suffix-tape` bought
is off for exactly the document sizes an artist blocks out in.

## What it costs, measured

A 24-dab stroke on a FLAT document — no groups anywhere — whose items carry an
ordinary quadratic blend at k = 0.05, driven through the brick cache on an
M-series Mac. `resumed` is bricks answered from a seed, per dab:

| base stamps | 30 | 50 | 75 | 200 | 400 | **700** | 1000 | 2000 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ms/dab | 0.26 | 0.19 | **1.02** | **2.85** | **4.33** | **7.42** | 0.17 | 0.17 |
| bricks resumed/dab | 38.5 | 38.5 | **0** | **0** | **0** | **0** | 38.5 | 38.5 |

**A document of 784 stamps costs 8.33 ms a dab; one of 810 costs 0.166 ms** —
50x, on a document 3% larger. 8.33 ms is twice the whole 4.17 ms interactive
frame share, per dab, in the middle of a blockout.

The boundaries are the formula's, to the node. `n_eff` is the node count
INCLUDING the dabs added so far, so a 24-dab stroke from base B runs
`B+1 .. B+24`:

| base | node range | prediction | resumed/dab |
|---|---|---|---:|
| 51 | 52..75 | constant | 38.5 |
| 60 | 61..84 | crosses 75 mid-stroke | 25.0 |
| 70 | 71..94 | mostly growing | 7.5 |
| 780 | 781..804 | growing | 0.0 |
| 790 | 791..814 | crosses 808 mid-stroke | 9.0 |
| 800 | 801..824 | mostly clamped | 26.0 |
| 810 | 811..834 | clamped | 38.5 |

## Why nothing caught it

**Every SDF fixture in the device suite uses HARD blends.**
`SceneBuilder.addStampNode` and `addStrokeDabNode` never call
`clay_item_set_blend`, and the default is `BlendProfile::Hard` with `k = 0`. For
a hard blend `profile_chain_pad` returns 0, so the pad is a constant zero, the
envelope never engages, and the gate cannot see any of this. `sdf_stamp_bricks`
and `sdf_stroke_bricks` are measuring a document no sculptor makes: the clay and
build brushes are smooth by default.

It was found by accident, chasing an unrelated ratio, and only because one
fixture happened to carry `CLAY_BLEND_QUADRATIC`.

## What Changes

- **The pad becomes piecewise constant.** The envelope's value is quantised —
  rounded UP to a coarse step — so it changes a handful of times across the
  whole band instead of on every node. Rounding up is sound in the direction
  that matters: a larger pad keeps MORE items in a brick's culled tape, and the
  band-clamped result is unchanged by keeping items that could not have changed
  it. The envelope is a measured fit, not an exact quantity, and it already
  carries a margin.
- A step boundary still costs one full refill, which is correct: the pad really
  did change. What goes is paying that on every dab.
- **The seed gate stays exact equality.** It is right: a seed taken under a
  different pad was continued from a different field. This change makes the pad
  hold still; it does not loosen what a seed promises.
- **The device fixtures gain smooth blends**, because a suite whose SDF cases
  are all hard-blended cannot see the pad at all.

## Capabilities

### New Capabilities
None.

### Modified Capabilities
- `brick-cache`: what a stored seed's validity depends on — that the pad a seed
  is keyed by holds still across an append except at a stated step.
- `device-gate`: the SDF stamp and stroke cases are measured with a smooth
  blend, since a hard-blended fixture cannot reach the chain pad.

## Impact

- `src/scene/bounds.cpp` — `chain_pad_envelope`, and the quantum it rounds to.
- `include/clay/scene/bounds.h` — what the envelope promises about stability.
- `tests/unit/` — the pad is constant across an append except at a step; a
  quantised pad is never SMALLER than the fit it replaces; band-clamped results
  are unchanged.
- `tests/device/Shared/SceneBuilder.swift` — a smooth-blended stamp and dab.
- `tests/device/Measure/LatencyCases.swift` — the smooth-blended cases.
- `docs/09-brush-latency-and-coverage.md` — the band, and what the suite was
  blind to.

## Non-goals

**Narrowing the pad.** How WIDE the pad should be is `narrow-the-chain-pad`.
This change is only about how OFTEN it moves — a narrower pad that still moved
every dab would leave the resume just as dead.

**Loosening the seed gate.** Accepting a seed whose pad merely differs would be
wrong, and accepting one whose pad is LARGER does not help: the pad grows on
append, so a stored seed's pad is smaller than the current one, computed from
strictly fewer items.

**The `>= 808` regime.** Above the clamp the pad is already constant and the
resume already works. Nothing there changes.
