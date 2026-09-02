# Proposal: the cull plan should not re-derive what it already knows

## Why

`CullIndex::plan` asks every cached entry the survive test as the compiler
writes it:

```cpp
if (!e.local || e.bound.is_infinite() || e.bound.intersects(test)) kept.push_back(e);
```

Two of those three clauses cannot change. `local` is decided when the entry is
built and `bound` never moves, so `!e.local || e.bound.is_infinite()` is a
constant per entry that the scan re-derives on every plan — and `is_infinite`
calls `empty`, and `intersects` calls `empty` on both operands, so the constant
part costs more than the question actually being asked.

The layout compounds it. An `Entry` is 40 bytes of which the scan reads 24: the
`const Node*` and the `NodeId` beside the bound are what the SURVIVORS carry,
not what the test needs, and striding over them is bandwidth the scan spends
without using.

Measured on a 24-core Linux box, over the fixture below at 50,000 items:

| | ms |
|---|---:|
| `plan()` | 0.1373 |
| the predicate loop alone, no plan machinery | 0.1379 |

The scan IS the plan — the map insert, the survivor vector and their
destruction are together under half a percent — so it is the only thing in
`plan` worth attacking.

## What changes

**Each chain keeps a packed box per entry, parallel to `entries`.** It holds
the entry's own bound, or an infinite box when the entry can never be culled —
which is exactly what `!local || bound.is_infinite()` decides, so deciding it
once at build time collapses the three-clause test to one box intersection over
a 24 B record.

**The intersection drops `Aabb::intersects`' two emptiness guards**, which is
exact once both operands discharge them: the entry's by the fold, the region's
by `plan` before the loop. An INFINITE region does not discharge its half — six
bare comparisons pass for every probe, including one folded from an entry whose
own bound is empty, which the predicate drops — so that region alone keeps the
predicate as written. An empty region needs no such fallback and the proposal's
spec delta says why.

**A benchmark whose document grows in EXTENT.** Every SDF fixture in
`benchmarks/bench_main.cpp` grows a unit sphere's DENSITY, where a dab's region
keeps a flat 28.3% of the items at 2 000, 10 000 and 50 000 alike. On such a
document `plan` is ~3% of a dab's cull and the per-brick compiles over its
survivors are the rest, so the fastest imaginable broad phase wins 3%. That is
a property of the fixture, not of the engine, and it is how
`add-item-spatial-index` came to be measured as a 590x faster query inside a
2.4x slower operation. `BM_CullPlanLocal{10000,50000}` grows the other axis.

## Impact

Measured against `main` on the same box, same run, both fixtures:

| | main | this | |
|---|---:|---:|---:|
| `plan`, 50 000 items, extent fixture | 0.1368 ms | 0.0258 ms | **5.3x** |
| `plan`, 50 000 items, density fixture | 0.1798 ms | 0.0706 ms | **2.5x** |
| `CullIndex` build, 50 000 | 2.60-2.80 ms | 2.64-2.65 ms | unchanged |
| `CullIndex` copy, 50 000 | 0.0652 ms | 0.1066 ms | +0.041 ms |
| `append_cached`, 50 000 | 0.00176 ms | 0.00273 ms | +0.001 ms |

The density fixture gains less because 28% of its entries are COPIED into the
survivor list, and that copy is untouched by any of this.

The two costs are the price of a second array: +24 B per entry, so +1.2 MB on a
50,000-item index, paid on the copy `append_cached` makes when a reader holds
the index. Both are far under what the scan saves — a stamp on the copying
branch nets -0.070 ms and on the in-place branch -0.110 ms — which is the
opposite of what a dynamic AABB tree measures there, where the tree's share of
the copy (+0.140 ms at 50 000) is the whole of the query saving.

## What it is not

**Not a spatial index.** The scan is still linear; this lowers its constant by
5.3x and leaves the slope exactly where `add-item-spatial-index` found it.
`BM_CullPlanLocal50000` is gated at 7x its 10 000 sibling and sits at 4.3x — a
regression gate, and the row a broad phase would have to flatten.

**Not a change to any survivor set.** The packed scan is held against the
predicate over four regions including both degenerate ones, and the existing
byte-identical tape corpus is unchanged.
