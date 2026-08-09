# Proposal: a consolidation policy, so SDF verbs compose into strokes

## Why

The SDF sculpting verbs are not missing. `Volume.relaxed` smooths under a
region with a falloff and a mask, `Volume.flattened` and `flattened_from`
flatten against a plane, `snakehook` pulls a tendril, `Layer.move_surface`
drags a surface, `moved_topologically_from` moves material along the form, and
`MaskField` is a complete mask brush — paint, stroke, smooth, expand, contract,
invert-within, and `to_field`. Those landed across 2026-08-07 and the roadmap
records each one.

What is missing is the ability to use them **more than once in a row**.

Every one of them follows the same pattern the roadmap names: *sample the
document, rewrite the stored samples under a region, hand back a volume*. That
is exactly right for one gesture and it does not chain, because the second call
samples the first call's **volume** rather than the document. Outside its band
a volume reports a lower bound rather than a distance, so the blend works from
the wrong value.

Both halves of this are already measured, by examples that fail if the numbers
stop holding:

- **hPolish** (`examples/28_hpolish.py`): one pass is clean and 1-Lipschitz.
  The second pass takes the declared Lipschitz from **1.00 to 14.0** whatever
  the falloff, and by the third the form is visibly corrupt rather than merely
  expensive. The example's own docstring concedes the consequence: hPolish is
  "a single-pass verb on an SDF layer" today.
- **Move** (`examples/27_move_strokes.py`): a stroke is many drags, each one
  another grab on the chain, each multiplying the declared Lipschitz. The safe
  step scale decays about **x0.615 per drag** — 18x the marching cost by six
  drags, **79x by nine**. Coalescing fixed frames within one drag, where the
  centre and radius are constant; it cannot fix a stroke, which moves the
  centre by definition.

So the honest statement of the gap is not "SDF layers cannot smooth". It is
that **a sculpting app is made of strokes, and these verbs are gestures.** An
artist does not polish once; they polish, look, polish again. Today the second
look is more expensive than the first and the third is wrong.

## What already exists

`clay_item_volume_from_document` collapses an edit list into a single volume,
so the *mechanism* for consolidating is present and tested. Nothing here needs
a new baking primitive.

What is absent is a **policy**: when consolidation should happen, what it
costs, and how a baked region rejoins an edit list that is still parametric
everywhere else.

## Approach

Three questions, and the change exists to answer them rather than to add a verb.

**When.** A host cannot be expected to guess. The engine already knows the two
things that matter — the declared Lipschitz of the current chain and the safe
step scale it implies — so it can say when a chain has degraded past a
threshold. Whether consolidation is then automatic or advisory is the decision.

**What it costs.** Consolidation is a bake: it fixes a resolution, spends
memory, and discards the procedural history of what it absorbs. `Volume`
already reports `megabytes`, `brick_count`, `sample_count` and
`sample_lipschitz`, so the cost is measurable before it is paid. It should be
reported, not silently incurred.

**How the result rejoins.** This is the substantive one. After consolidating a
region, the document holds a volume where it held items. The parameters of
those items are gone, so a host that offered "edit this sphere's radius" can no
longer offer it there. The policy must state whether consolidation is scoped to
a region, a layer or a stroke, and what a host can still promise afterwards.

## The tension this has to resolve

Consolidating early keeps the field cheap and loses editability. Consolidating
late keeps everything editable and makes the marcher crawl. Neither end is
right, and the reason the answer is not obvious is that the two costs are paid
by different people — the raymarcher pays for the deep chain, and the artist
pays for the early bake.

A defensible answer probably distinguishes **strokes from gestures**: within a
stroke, consolidate eagerly, because the artist has already committed to the
result and is watching for the shape rather than the parameters; between
strokes, keep the history. The proposal should test that framing rather than
assume it.

## Open questions

- Whether the trigger is a Lipschitz threshold, a chain depth, a memory budget,
  or a host call.
- Whether consolidation is undoable as one command — it should be, and its
  inverse has to restore the items it absorbed, which means the undo record
  holds them.
- Whether a consolidated region can be re-expanded, or is one-way.
- How this interacts with `add-multi-resolution`, if that lands: a bake fixes a
  resolution, and levels would give it one to fix.

## Impact

`sdf-kernels` gains the statement that a chain's declared Lipschitz is the
signal. `scene-model` gains the command and its inverse. `c-abi` and
`python-bindings` gain the surface and the cost report. Additive: a document
that never consolidates behaves exactly as today, which is also why the
degradation above is a live problem rather than a hypothetical.
