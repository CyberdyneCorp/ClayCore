# Proposal: a stroke should not rebuild the cull index per dab

## Why

The cull index caches, per document revision, every bound a per-brick culled
compile consults. A stroke appends one item per stamp, and every stamp bumps
the revision, so the index is rebuilt from scratch — walking every node in the
document, recomputing bounds that did not move — to add one item.

Measured, dabs spread evenly over a sphere:

| items | index build | of which the pad walk | of which bounds |
|---:|---:|---:|---:|
| 5,000 | 0.43 ms | 0.02 ms | 0.41 ms |
| 20,000 | 1.16 ms | 0.17 ms | 1.00 ms |
| 50,000 | **2.45 ms** | 0.15 ms | **2.29 ms** |

94% of it is bounds that an append does not change. Against a 4.17 ms
interactive budget, at 50,000 items the index rebuild alone is 59% of the frame.

This is the same disease `compile_document_append` cured for the tape (#294),
in the other cache the same edit invalidates. It is also the term left standing
once the evaluation half of #306 lands: seeding a suffix removes the per-brick
compile and the evaluation, and then the index build IS the dab.

## What

**`CullIndex::append(appended)`** extends an index in place for a document that
has gained those items at the tail of its last visible SDF layer's roots, and
nothing else. It computes the new entries' bounds, extends the affected chains,
and re-derives the pad.

Refuses — leaving the index untouched — when it cannot be sure that is what
happened: no such layer, no chain over its roots, or the items not actually at
the tail in order. The same terms `compile_document_append` refuses on, and for
the same reason: a refusal costs the rebuild the caller would have paid anyway,
and a wrong extension is silent.

**The C ABI takes it.** `Doc::cull_index()` extends the cached index instead of
rebuilding when the append log sits exactly on top of it. The index gets its OWN
log rather than sharing the tape's, because that one is single-consumer by
construction — it resets its base when the tape absorbs it, which is what keeps
the next append reusable — so a shared log would be spent about half the time
depending on which cache the host asked for first.

## Impact

| items | rebuild | append | |
|---:|---:|---:|---:|
| 5,000 | 0.238 ms | 0.010 ms | 24x |
| 20,000 | 0.981 ms | 0.070 ms | 14x |
| 50,000 | 2.421 ms | 0.132 ms | **18x** |

What is left is the pad walk, which is a cheap `cmax` over the flat node map and
is recomputed rather than tracked — keeping the two maxima separately to update
in place would store a sum of maxima where the pad is a maximum of sums, which
is safe (being larger) but no longer what a fresh build says.

## The bug the corpus caught

The first implementation extended only the last visible SDF layer's chain. The
adversarial corpus has an **instanced layer** — one `SdfContent` compiled under
two layers — so one roots vector is two chains, each with its own bounds because
`item_geometry_bound` reads the layer's transform and mirror. Extending one left
the other describing a document that no longer existed, which showed up as a
55-instruction tape where a rebuild gave 59. Every chain over the root list is
extended now.

## Non-goals

**Anything that is not an append.** An edit in the middle, a reorder, a removal:
the log is cleared and the index is rebuilt, exactly as the tape's is.

**Sharing one append log between the two caches.** It would need each consumer
to track its own base and the log to reset only when every consumer is current —
a change to the most sensitive caching code in the ABI, for a saving neither
cache is waiting on.
