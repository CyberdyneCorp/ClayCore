# Proposal: a resumed refill should carry colour

## Why

`clay_brick_cache_eval_requests` resumes from its own previous output, so a dab
costs what the dab adds — but only for distances. A caller asking for colour
took the full walk, which is most of the difference for a host that paints while
it sculpts: 8.12 ms against 0.15 ms at 20,000 items on the same fixture.

The gate was real rather than lazy. What the accumulator IS decides what a seed
must carry: a distance-only walk folds one float a point, a coloured one folds a
`CTapeValue`, and continuing a coloured fold from a distance alone would fold
every combine against black — a wrong answer rather than a missing one.

## What

The seed carries the colour too. `eval_points_seeded` takes `seed_rgb` beside
`seed` and runs the coloured walk when the caller wants colour back AND supplied
the colour the prefix reached; the store keeps both planes; the refill serves
coloured requests from them.

A caller asking for colour back WITHOUT supplying it gets distances only rather
than a fold against black — the one shape of this that would be silently wrong.

A brick refilled without colour cannot serve a coloured request, and falls back.

## Impact

12 bricks on the pole of a sculpted sphere, one dab, colour requested:

| edit-list length | before | after | |
|---:|---:|---:|---:|
| 1,000 | 0.460 ms | **0.026 ms** | 18x |
| 5,000 | 2.109 ms | **0.059 ms** | 36x |
| 20,000 | 8.124 ms | **0.150 ms** | 54x |

## Memory

The budget becomes BYTES rather than a brick count: with colour a brick carries
four times the floats, so a count would have meant two very different ceilings
depending on what the host asked for. 64 MB is 16,384 distance-only bricks of a
dim-8 cache, or 4,096 coloured ones.

## Non-goals

**Gradients.** Four taps of the whole field is not something one accumulator can
be continued into.

**Multiple visible SDF layers**, which is the other half of widening this and
needs its own change: with a second layer the tape holds TWO accumulators at the
checkpoint — the document's and the layer's — and a refill's single output does
not carry both.
