# Proposal: an edit reaches from the node it names, not from that node's root

## Why

`node_command_bound` answers "where does this edit land?" by walking UP to the
node's root ancestor and returning that root's whole influence bound
(`src/scene/commands.cpp:317`). So adjusting one item inside a group dirties the
entire group, however small the item and however far it is from the rest.

The reason it was written that way is recorded, and it is a real constraint: a
group's blend spreads a child's influence past the child's own box. But the
whole subtree is a much larger answer than that constraint needs. The tight
answer is the child's own bound dilated by the blend support of each group on
the path from the child up to its root — the ancestors' supports, not the
siblings' geometry.

Because it is loose, it also switches off the frontier path (#360). The frontier
is the root ordinal the edit dirties from; with everything under one group that
ordinal is 0, so no brick can keep its prefix and every seed is dropped.

## Where it is paid, which is not where it was first looked for

The first measurement of this took the wrong reading, and the correction is
worth keeping because it decides what the fix is worth.

A gizmo drag does NOT show it. Measured on the reference iPad (`iPad15,5`, ABI
0.60.0, p95 at 1000 items): a drag frame costs 14.28 ms with the dragged node at
the layer root and 13.74 ms with it inside a group — the same, to inside the
noise. A drag walks, so each frame refills roughly what the frame before it
dirtied, and those bricks lose their seeds either way. A host that only ever
evaluates the dirty set never asks for the seeds the wide bound retired.

It is paid by THE NEXT EDIT. The drag inside a group retires every seed in the
group, including all the ones nowhere near the gizmo, and nothing pays for that
until something else is edited. On the same device, an ordinary stamp following
one drag frame:

| stamp after a drag frame, p95 at 1000 items | flat | inside a group |
|---|---:|---:|
| `sdf_stamp_after_drag_bricks` / `..._group_drag_bricks` | **1.10 ms** | **4.32 ms** |
| bricks refilled per stamp | 12.6 | 12.5 |

**3.8-3.9x, at an identical brick count** — 3.91x on the run above and 3.82x on
an independent confirming run. The region is held fixed by construction, so the
whole of the difference is bricks that had to walk the edit list instead of
resuming. It takes an ordinary stamp from comfortably
inside the 4.17 ms frame share to outside it, which is the concrete cost: after
an artist moves anything in a grouped document, the next stroke stops being
interactive.

**Both are needed, and that took an A/B to establish.** The tighter bound alone
moved nothing while the pad still changed on every append — every seed died to
the pad gate before the bound could save it. With `hold-the-cull-pad-still`
landed, and A/B'd against the same tree carrying only the pad fix: a grouped
document under a host that re-evaluates its visible set goes from 0 bricks
resumed and 44.17 ms a frame to **285.2 resumed and 32.85 ms — 1.34x**, with
the flat control unchanged. This change should land after the pad fix, or the
measurement will say it does nothing.

The same effect appears wherever a host evaluates more than the dirty set.
Measured on an M2 Max over a host loop that re-evaluates its visible set each
frame, 512 bricks at 1000 items: flat resumes 377 of them and costs 23.7 ms,
grouped resumes none and costs 44.7 ms.

## What Changes

- A node command's reach becomes the **named node's own influence bound,
  dilated up the ancestor path**: the node's bound, then at each enclosing
  group in turn dilated by that group's blend support, up to the root. The
  existing union over every layer sharing the content is unchanged, and so is
  the infinite answer for a subtree whose combine is non-local.
- The frontier ordinal an edit dirties from is unaffected in shape — it is
  still the root ordinal — but the region it applies to is now small enough
  that seeds outside it survive.
- **No ABI change and no new entry point.** This is the region three existing
  callers already ask for: the command funnel, the undo/redo bound, and
  `clay_layer_node_influence_bound`.

## Capabilities

### New Capabilities
None.

### Modified Capabilities
- `c-abi`: "Undo reports the region it changed" states the root-ancestor rule as
  the conservative answer for a node inside a group. It becomes the
  path-dilated rule, which is conservative in the same sense and tighter.
- `scene-model`: "Influence bounds" gains what a bound means for a node read
  through its ancestors, so the tighter rule is stated where bounds are defined
  rather than only where undo reports one.

## Impact

- `src/scene/commands.cpp` — `node_command_bound`, and the `root_ancestor` walk
  it uses becomes a walk that accumulates support instead of discarding the
  child.
- `src/scene/bounds.cpp` / `include/clay/scene/bounds.h` — the ancestor-path
  dilation, beside `node_influence_bound` which already computes a group's
  support the same way.
- `bindings/c/clay_c.cpp` — nothing, except that `command_frontier` now reaches
  a region worth keeping seeds in.
- `tests/unit/` — the conservativeness property test, run on a grouped
  document; the existing undo-bound scenarios.
- `docs/09-brush-latency-and-coverage.md` — the transform row, once the device
  cases exist.

## Non-goals

**Loosening what a bound promises.** The bound stays conservative in the
band-clamped sense `scene-model` already states: outside it, band-clamped
values are unaffected. This change makes it smaller, never smaller than the
truth.

**The non-local cases.** A subtree carrying an intersect, an infinite grid
repeat or an unbounded primitive still reports infinite, unchanged. The
path-dilation applies to the local case only, which is the one
`item_influence_is_local` already isolates.

**The layer-parameter commands.** A layer transform, mirror or radial edit
reaches the whole layer and this change says nothing new about them — that is
`drag-a-layer-without-a-refill`.
