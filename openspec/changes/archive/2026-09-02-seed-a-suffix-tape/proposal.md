# Proposal: a dab should cost what the dab adds

## Why

A dirty brick re-evaluates every surviving item of the edit list over its
samples, including the thousands that have not changed since the last revision.
So a dab's cost follows everything the artist has already sculpted. Measured at
a 0.05 voxel, one dab into 12 bricks, dabs spread evenly over a sphere so that
per-brick culling is working (#306):

| items | culled instrs, 12 bricks | dab |
|---:|---:|---:|
| 200 | 316 | 0.23 ms |
| 5,000 | 8,260 | 1.95 ms |
| 50,000 | 82,538 | **18.07 ms** |

The only answer today is `consolidate_layer`, which costs 18.8 s at 5,000 items
and is explicitly one-way: it collapses the edit list into samples and
`consolidate.h` offers no un-bake. The representation's headline advantage is
that the items stay editable, and the only way to afford them is to stop.

## What

**`scene::compile_layer_suffix`** — the same compile `compile_document_append`
performs, without the prefix. It emits only the instructions for the appended
items: a tape that expects the accumulator the items in front of it left, and
folds onto it.

**`eval::eval_points_seeded`** — the blocked walk, started with that accumulator
on the stack instead of an empty one.

Together they answer "what does this dab add?" instead of "what does this
document say?".

## Why it is exact

The compiler emits each item's contribution as a self-contained expression and
folds it into ONE running accumulator, so after every item the stack holds
exactly one value. Measured on the tape: a flat chain is `prim, prim, COMBINE,
prim, COMBINE, …` at depth 2, and a chain under a layer mirror emits `prim,
prim, COMBINE` per item and still returns to depth 1 at every item boundary.

So continuing from that value is not an approximation of replaying the chain —
it is the same instructions in the same order over the same floats, with the
part already folded represented by the number it produced. Given an exact seed
the result is **bit-identical**, and the test asserts identity rather than a
tolerance.

`TapeCheckpoint` already names the places where that is true, so the validity
question is one `compile_document_append` answers rather than a new one — and it
comes with the right instinct: refuse wherever it is not certain, because a
refusal costs the full evaluation the caller would have paid anyway and a wrong
reuse is silent.

## Impact

Per dab, 12 bricks, seed already in hand:

| items | full walk | seeded suffix | |
|---:|---:|---:|---:|
| 1,000 | 0.63 ms | **0.03 ms** | 24x |
| 5,000 | 2.28 ms | 0.03 ms | 91x |
| 20,000 | 7.76 ms | 0.02 ms | 346x |
| 50,000 | 18.79 ms | **0.02 ms** | 845x |

The suffix is **two instructions** whatever the document holds, which is the
point: the cost stops following the document. `BM_DabSuffixSeeded` against
`BM_DabFullWalk` reads 0.053 ms against 6.66 ms at 10,000 dabs, and the gap
widens with the fixture rather than being a fixed percentage.

## What this does NOT do

**It does not hold the seed.** Nothing stores the per-brick accumulator yet, so
nothing in the shipping paths is faster; the benchmark keeps seeds in a local
buffer, which is what a cache would do. That cache — its key, its invalidation
rules, its memory budget — is the follow-up this change exists to make possible,
and it is the part with the design questions.

**The brick cache cannot be that store.** Its values are `fp16` clamped to
`±band` (`brick/cache.h`), and Inside/Outside bricks hold nothing at all — so
the accumulator needs fp32 storage of its own, and there is none where a growing
dab most often lands.

**CPU only.** The seeded walk is a variant of the CPU blocked evaluator, not a
change to `ctape_eval` or the kernel dialect, so no backend parity moves.

**Distance only.** A seed is one float a point; a gradient would need the
prefix's four taps and a colour would need a colour.
