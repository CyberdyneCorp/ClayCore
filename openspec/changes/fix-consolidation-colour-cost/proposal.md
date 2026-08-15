# Proposal: a grey layer should not pay for colour

## Why

The v0.31.0 device gate failed on the reference iPad. `sdf_consolidate` came
back **1.75× slower** than the v0.30.0 baseline and over its `operation`
budget — 916 ms p95 against a 786 ms budget, where v0.30.0 measured 524 ms.
`mask_extrude` regressed 1.48× on the same run. No voxel case moved.

Bisected on the Mac with a harness driving `clay_layer_consolidate` through
the C ABI, same document shape as the device case, median of five with the
document rebuilt untimed between samples:

| commit | | 1000 stamps |
|---|---|---|
| `196d403` | before the colour channel | 248.2 ms |
| `ac7460a` | the tape reads a volume's colour | 271.9 ms |
| `85f1679` | **the producers write colour** | **1115.5 ms** |
| `71118c1` | after `e02d8b7` pooled the colour pass | 375.3 ms |
| `b1868d4` | main | 389.1 ms |

Everything that landed after the colour channel — quad export, mesh brushes,
rasterize, both thread-pool commits — accounts for 4% of it. The thread pool
is not implicated.

`85f1679` made consolidation 4.5× slower. `e02d8b7` caught that and pooled the
colour pass, recovering most of it — and stopped there, at 375 ms against a
248 ms starting point. **It fixed 78% of the regression it introduced and
shipped the other 22%**, which compounds to 1.57× on the Mac and the 1.75× the
iPad measured.

That is visible in `e02d8b7`'s own message: *"118 ms pooled against 817 ms
serial, ~7×, better than before this change went in."* Both numbers are the
new code — pooled against serial, within the branch. Nothing compared against
`196d403`, so a 51% residual read as a 7× win. The benchmark gate could not
have said otherwise either: `BM_ConsolidateGrownDoc` is checked against
`BM_ConsolidateSerialGrownDoc`, a ratio between two paths that both pay for
colour.

## What

**The colour pass runs unconditionally, and it is a second full evaluation of
the tape over every surviving sample.** `consolidate.cpp:199` calls
`fill_colors_blocks` whether or not anything in the layer carries a colour.
`Node::color` defaults to `cf3(0.7, 0.7, 0.7)`, so there is no "has colour"
flag to skip on — every sample of a plain grey sculpt is evaluated twice to
recover a constant.

**A uniformly coloured layer needs no colour channel at all.** The node's own
colour already answers for it, which is what the spec says: *"the node's
colour SHALL remain the answer outside the sampled box and for a volume with
no colour."* The bake already reads that colour off the absorbed set three
lines further down. So the pass is earned only when the absorbed set holds two
or more distinct colours — the "skin and armour" case the requirement exists
for — and it is pure waste otherwise.

The predicate is O(nodes) against a pass that is O(samples).

**Observable behaviour does not change.** A layer with one colour consolidates
to a volume with no colour channel and a node carrying that colour, which
evaluates identically and is what v0.30.0 produced. A layer with two colours
takes the pass exactly as it does today.

## Non-goals

- **The 10% from `ac7460a` stays.** `ctape_prim_dist` gained an out-parameter
  the caller seeds before every prim evaluation, on every backend. That is the
  cost of the design decision recorded in that commit — an out-parameter
  rather than widening the return type to `CTapeValue`, which would have
  charged all five backends more. It is named here as a known cost rather than
  chased.
- **Not a re-baseline of the budget.** `sdf_consolidate`'s budget is not moved
  to fit the new number. The budget is what says a case is too slow whether or
  not it regressed; fitting it to the measurement is what the class exists to
  prevent.
- **`mask_extrude` is not fixed here, and the guess about it was wrong.** This
  said the same fix was expected to cover it and called that inference; the
  device re-run then measured it unmoved (3695 -> 3787 ms), and a bisect put
  its regression at `ac7460a` — the tape out-parameter above — rather than at
  the colour pass. See tasks 3b. It needs its own change, and it is now over
  budget rather than merely regressed, so that change is not optional.
