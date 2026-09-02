# Proposal: extending the cull index should cost the dab, not the document

## Why

#309 stopped a stroke rebuilding the cull index per stamp, and #306 stopped a
dab re-evaluating what was already sculpted. What is left is the extension
itself, and it still walks the document twice per stamp. This is #306's Risk 6
— *"once evaluation is reduced, cull+compile becomes the next bottleneck"* —
arriving on schedule.

Measured on a twelve-core Linux box, 20,000-item document, load average 0.27
before and 0.38 after:

| | ms |
|---|---:|
| `BM_BrickRefillMoving20000` — a whole resumed dab, 4 bricks | 0.171 |
| `BM_CullIndexAppend` — extending the index for that dab | 0.086 |
| `BM_CullIndexRebuild` — what the append already avoids | 1.11 |

**The cull index is about half of a resumed dab**, and it is on the full path
too — it simply was not worth attacking while evaluation dominated. On the
steadier fixture this change gives those two benchmarks (see the note below) the
same box reads 0.123 ms and 0.054 ms, plus 0.043 ms of copy the append
benchmark never measured — so it is nearer 79% than half.

Two costs, and they scale differently:

- **The deep copy.** `clay_document::cull_index_locked` takes the append fast
  path by copying the cached index, because another thread may be holding the
  old one against a plan it already made. `CullIndex` holds a `vector<Chain>`,
  each with its own entries, so that is O(document) allocation and copy to add
  one item's bounds.
- **The pad walk.** `CullIndex::append` recomputes the touched layer's cull pad
  with a full walk of its flat node map. #309's own comment defends this as *"a
  cheap walk, 0.15 ms of the 2.45 this avoids"* — true against a rebuild, false
  against a 0.171 ms dab, where it is essentially the whole of an append.

## What

**The pad is kept as its two terms, per layer, and raised from the appended
subtree.** #309 rejected tracking it because the pad is a MAXIMUM OF SUMS and
one pair of global maxima would store a SUM OF MAXIMA — larger, so safe to cull
with, but no longer the number a fresh build reports. That reasoning holds; the
way out is that per layer the two are the same number. `CullIndex` keeps a
`CullPadTerms` per visible SDF layer, folds them into `pad_` as a maximum of
sums, and an append raises only the terms of the layers it touched, from the
subtree it added — including the children of an invisible group, because the pad
folds over the flat node map and does not care what a build descends into.

`scene::cull_pad_terms(node, layer)` is the one definition of either term, and
the whole-layer walk is a fold of it, so a new feathered shape or a new dragging
combine cannot reach one and not the other.

**An append the map does not corroborate is refused.** The terms are raised from
the named subtree, so a node map that gained anything else would leave them
BELOW a fresh build's — and a pad that is too small plans against too small a
region, which is the one direction that loses items a brick needed. Each layer's
pad slot carries the map size its terms were taken at; an append whose subtree
does not account for the whole growth refuses, and refusing costs the rebuild
the caller would have done anyway.

**The copy is taken only when someone is holding the index.** Every reader takes
its handle from `cull_index_locked` under the cache mutex and holds it while it
reads, so a use count of one, observed under that mutex, means no snapshot
exists and none can appear until the lock is released. Then the append extends
the cached index in place and the copy is not made at all. A refused append
leaves the index untouched — its contract — so the rebuild after one is still
correct.

Copy-on-write chains (`shared_ptr<const Chain>`) were the third direction in the
issue and are **not** taken: the chain an append extends is the last layer's
root list, which in a sculpt IS the document, so cloning it is the copy that was
already being paid. It would help a document made of many large chains and cost
an indirection on every plan; measure it against such a document before
believing it.

## Impact

Medians of 7 repetitions, twelve-core Linux box. The box is shared, so both
sides were measured back to back on a quiet one and the load average is quoted
either side: **1.07 → 1.05** before, **1.02 → 1.02** after.

| | before | after | |
|---|---:|---:|---:|
| `BM_CullIndexAppend` — an append, in place | 0.0542 ms | **0.000258 ms** | 210x |
| `BM_CullIndexAppendShared` — the copy the ABI falls back to, plus the append | 0.1050 ms | 0.0432 ms | 2.4x |
| `BM_BrickRefillMoving20000` — the whole resumed dab | 0.1233 ms | **0.00299 ms** | 41x |
| `BM_CullIndexRebuild` — the constructor, untouched | 1.062 ms | 1.059 ms | 1.00x |

The two rows at the top separate the two costs cleanly: the copy is what is left
of `Shared` once the append is free, 0.043 ms, and the pad walk is what is left
of `Append` before it, 0.054 ms. Together 0.097 ms of a 0.123 ms dab — 79% of
it, rather than the half the issue estimated from a noisier fixture. The dab's
`refilled_frac` is unchanged at 0.0004: the same bricks resumed, 41x less spent
getting to them.

**The slope, which is the point.** The same append onto a document a tenth the
size:

| | 20,000 nodes | 2,000 nodes | ratio |
|---|---:|---:|---:|
| before | 0.0542 ms | 0.00454 ms | **11.9x** |
| after | 0.000258 ms | 0.000257 ms | **1.00x** |

An append now costs what the append adds. `tools/check_bench.py` gates that
ratio at 4.0x, which the unfixed code reads 12.5x against.

## Note on the benchmarks

`BM_CullIndexAppend`, its new small-document twin and
`BM_BrickRefillMoving5000/20000` now pin their iteration count, and the append
pair rebuilds its fixture every 256 iterations and warms it with one untimed
append. Both are the same defect: every iteration appends an item to the
document it is measuring, so left to the clock the fixture grows by however many
iterations the machine got through — and the first append after a fresh index
doubles a 20,000-entry vector, a megabyte of copy that a stroke pays once and
the loop would otherwise pay every reset. A benchmark whose fixture is a
property of the machine cannot hold a slope.

## Non-goals

**Copy-on-write chains.** See above.

**Anything that is not an append.** Unchanged: the log is cleared and the index
is rebuilt.
