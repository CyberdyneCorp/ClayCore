## Why

Issue #451: an intersect drag costs more than the same drag with a subtract.
#454 and #459 took it from 118x to ~57x and named what was left — one walk of
the layer per frame, 96% of it `item_geometry_bound` inside
`layer_influence_extent`. Measured on `main` at 0.0669 ms against a 0.0003 ms
subtract control, on a 401-item layer, one `set_transform` per drag frame.

The walk happens because an intersect is bounded by its layer's EXTENT, and any
edit might have changed that extent, so it is recomputed on every edit.

## The obvious cache is the wrong one, and it was measured being wrong

First built: keep the extent plus the item achieving each of its six faces. An
item that holds no face out and still fits inside cannot have changed the union,
so it moves for one item bound and six comparisons.

It is exact, it passed an exhaustive per-command gate, and **its fast path never
fired**: 201 walks over a 200-frame drag. The thing a host drags across a form
is a boolean OPERAND, and an operand big enough to cut something sticks out of
it — so it defines a face on every frame and every frame walked. The fixture in
the issue has the dragged box spanning y ∈ [0.55, 1.25] against a form reaching
1.1. Nothing about the timings said so; the walk counter did.

## What Changes

- `scene::LayerExtentCache` holds the union of every visible item **except
  one**, plus that item's box folded back in. Editing the held-out item is then
  one item bound and a union, whatever it did — grow, shrink, or leave the
  layer's silhouette — because its old box was never in the union to begin with.
  An edit to anyone else re-walks holding **them** out instead, so a drag pays
  one walk on its first frame and none after.
- Nothing is computed when the cache is told of an edit. Most layers hold no
  intersect and never have their extent asked for; an eager version made every
  one of them walk once per edit, which the "a layer with no intersect never
  walks" gate caught as 1 walk where it demands 0.
- `scene::command_edited_item` decides, from the command alone, whether an edit
  is confined to one item. The wiring and the exhaustive gate both call it, so
  the gate tests the rule rather than a second copy of it.
- `clay_document_extent_stats` reports reuses as the queries actually answered
  from the cache. It previously also counted every edit whose bound happened not
  to walk, and so reported 62 reuses on a layer whose extent was never needed.

## Result

Over the issue's 200-frame intersect drag:

| | one drag frame | layer walks |
|---|---:|---:|
| subtract (control) | 0.0003 ms | 0 |
| intersect on `main` | 0.0669 ms | 200 |
| **intersect, this change** | **0.0003 ms** | **2** |

The intersect drag now costs what the subtract control costs. **This closes
#451.**

## Capabilities

### Modified Capabilities
- `scene-model`: a layer's extent can be kept across an edit.

## Impact

- `include/clay/scene/bounds.h`, `src/scene/bounds.cpp` — the cache.
- `include/clay/scene/commands.h`, `src/scene/commands.cpp` — `command_edited_item`.
- `bindings/c/clay_c.cpp` — the wiring and the corrected reuse count.
- No ABI change: no signature moves, and `clay_extent_stats` keeps its shape.
