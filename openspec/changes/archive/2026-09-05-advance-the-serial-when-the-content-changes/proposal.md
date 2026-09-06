## Why

Issue #451: an intersect drag costs 1.17x more on v0.78.0 than v0.73.0. #454
fixed most of it and named the residual:

> "`apply_edit` still takes the bound on both sides, so a drag pays two.
> Removing that needs a revision-scoped cache on the ABI's document."

Reproduced on `main`, one `set_transform` per drag frame on a 401-item layer:

| | one drag frame |
|---|---:|
| subtract (control) | 0.0003 ms |
| **intersect** | **0.0353 ms** |

**118x**, and 96% of it is `item_geometry_bound` inside `layer_influence_extent`
(the bare `nodes()` iteration is 4%).

## The suggested fix cannot work, and fails silently

**`perform_edit` does not bump `revision`** — `touch_region` does, afterwards.
So inside one `apply_edit` the two bound calls see the SAME revision, and a memo
keyed on it answers the second with the FIRST's geometry: a bound that is too
small, which is under-invalidation and shows up as stale bricks rather than as
an error.

That is not a hypothetical. It was built first: it measured **0.0348 -> 0.0167
ms**, exactly the 2x the note predicts, and every existing test passed. What
caught it was making the memo report its own hit count — over a 20-frame drag it
read **20 walks, 0 reuses**, one walk a frame with no LEGITIMATE reuse, which is
only possible if both calls shared a key. `CullIndex` is keyed on the same
revision and would have hit the identical wall.

## What Changes

- `scene::Document::content_serial`, advanced inside **`scene::apply`** — the
  one funnel every command-based mutation passes through. Ordinary edits, undo,
  redo and a replayed journal all reach it, so nothing is enumerated and a
  command added later cannot be missed. A mutation made WITHOUT a command (a
  consolidation installing a volume) does not reach it, so the binding that owns
  such a path advances it where it already invalidates, in `touch()`.
- The ABI's extent memo keys on that serial instead of the revision. The two
  calls now land either side of the apply, and the memo left after one edit is
  the one the next edit opens with — so a drag walks its layer **once a frame
  rather than twice**.
- `clay_document_extent_stats` reports the walks and the ones the memo saved,
  because the bound is identical either way and nothing else can say whether it
  fired. It is the counter that caught the first attempt.

**`revision` is deliberately untouched.** The append log does arithmetic on it
(`appends_since` requires `append_at_ == now` and indexes by
`from - append_base_`, assuming one bump per append), and that log drives the
tape-prefix reuse — 0.54 ms against 10.3 ms at 50,000 items. Advancing the
revision inside `perform_edit` would have restarted that log on every append.
A separate serial leaves it alone.

## Result

Over a 20-frame intersect drag: **20 walks and 20 reuses**, one of the two bound
calls served from the memo every frame. Timed, interleaved on a loaded box:
0.0575 -> 0.034 ms, ~1.7x.

**This does not close #451.** One walk a frame remains, and closing it means
making the walk itself cheap — caching the per-item geometry bounds that are 96%
of it. That is a separate change, and it is now unblocked, because a cache keyed
on `content_serial` is correct across an edit where one keyed on `revision` is
not.

## Capabilities

### Modified Capabilities
- `scene-model`: a document carries a serial that advances when a command
  changes it.
- `c-abi`: what an intersect's bound costs is reportable.

## Impact

- `include/clay/scene/document.h`, `src/scene/commands.cpp`.
- `bindings/c/clay.h`, `clay_c.cpp` — the memo and the diagnostic.
- **ABI 0.80.0 -> 0.81.0.**
