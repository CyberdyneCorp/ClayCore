# Proposal: a host cannot ask what the document is without one layer

## Why

A live SDF transaction previews ONE layer. `clay_sdf_smooth_preview_delta_take`
hands a host the bricks that layer's working volume changed, and that is exactly
what a viewport needs — measured by the reporter at ~5 ms a dab against ~30 ms
for the snapshot path (#378).

What a host cannot do is draw it **beside anything else**.
`clay_brick_cache_eval_requests` takes a document and evaluates the hard union
of every visible SDF layer, and no entry point attributes a brick to the layer
it came from. So a host drawing the preview is drawing that layer alone, and
every other visible field layer disappears for the length of the gesture.

ClaySpaceDesktop handles it by refusing: a live gesture opens only when the
layer being smoothed is the ONLY visible SDF layer, and otherwise the tool falls
back to what it did before — held whole, applied at pointer-up. That is correct,
and it takes the feature away from exactly the documents subtools were added
for.

The three routes the reporter found, and why each is closed today:

- **Evaluate the rest of the document per frame.** `clay_eval_points` has no
  layer filter, so "the document without layer L" is not askable.
- **Hide the layer, sample the rest, restore.**
  `clay_document_set_layer_visible` is an edit, and an edit after
  `clay_sdf_smooth_begin` is one the commit correctly refuses. Doing it BEFORE
  begin costs a full-region evaluation per gesture and records history entries
  the host then has to fold out.
- **Compose in the preview cache.** Needs the other layers' distances at the
  preview lattice's sample points, which is the first bullet again.

## What this changes

The first route. A host evaluates "everything except L" once per gesture, and
takes the `min` itself — which is EXACT rather than an approximation, because
visible SDF layers compose by hard union and `min` is that union.

## The engine half already exists

This is the reason to do it now and to do it small.
`scene::compile_document_part(doc, active, below)` compiles one side of exactly
this split, is what the C ABI's own multi-layer brick refill already uses
(`bindings/c/clay_c.cpp:3733`), and is covered by `test_layer_prefix_tape.cpp`:

```cpp
// src/scene/tape_build.cpp — run_part
if (below ? layer.id == stop : layer.id != stop) {
    if (below && layer.id == stop) break;  // everything after it too
    continue;
}
...
if (have_acc) emit_combine(Op::Add, Blend{}, 0.0f);  // layers union hard
```

`below = true` emits the visible SDF layers BEFORE `active` and stops there;
`below = false` emits only `active`. Neither is "every layer except `active`",
and that is the whole gap: a two-way flag where the third case was never needed.

The two questions this split is easy to get wrong on are already answered in
that code, with the reasoning recorded beside them:

- **Both halves cull under the WHOLE DOCUMENT's pad**, not their own — "A part
  compiled under a smaller pad drops items the whole-document compile keeps, and
  then the two halves no longer sum to the whole."
- **The join is a hard `Op::Add`**, which is the union a whole-document compile
  emits between layers, and is the `min` the host is being told it may take.

So this change is a third case in an existing tested predicate, plus the ABI
surface to reach it. It is not a new evaluation path.

## What it does NOT do

- No layer attribution on a brick (the reporter's route 3). A brick is still the
  union; this lets a caller ask for a union over a different set of layers.
- No change to any existing entry point's behaviour or signature.
- Not a general layer-set filter. ONE excluded layer, because that is the shape a
  transaction has — a transaction previews one layer — and a set-valued filter
  invites a caller to rebuild the layer stack per frame, which is the cost this
  exists to avoid.

## Why refusing an unknown layer matters

The excluded layer is an INPUT a host derives from its own state, and a stale one
is the failure this API exists to prevent: excluding a layer that is not there
would evaluate the whole document and silently draw the layer the host meant to
hide, on top of the preview it drew itself. That is the doubled surface the
feature is supposed to make impossible, so an unknown layer is refused rather
than treated as "exclude nothing".
