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

> **Corrected during implementation, 2026-08-09.** That last sentence was
> wrong, and measuring it is what found the real gap. **Baking does not bound
> the Lipschitz.** Sampling the two-pass polish chain into a fresh volume gives
> samples varying at 14x the cell size at 0.04, 31x at 0.02 and 38x at 0.01 —
> a finer cell makes it *worse*, because there are then more cells across the
> same steep shell. Steepness is a property of the field, and resampling it
> onto a lattice reproduces it.
>
> What removes it is **redistancing**: replacing the baked samples with the
> distance to their own zero set. The tree had none — the roadmap says so
> explicitly, "no eikonal, no fast march, no redistancing anywhere" — so this
> change grew one (`field::redistance`, fast sweeping) and it is the primitive
> the row turned out to need. Without it the spec's "holds its declared
> Lipschitz within a stated bound" is unachievable by consolidating.
>
> The same measurement caught a second thing: `FieldVolume::sample` never
> measured what it stored, so `from_document` declared every bake 1-Lipschitz
> whatever it had just written. That made it unsound as a consolidation
> primitive — a 14x overclaim licenses exactly the overstep the declared bound
> exists to prevent — and it is fixed here with a regression test.

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

## Open questions — settled, with the reasoning in `tasks.md` and the headers

- **The trigger**: a safe-step-scale threshold, reported and never acted on, with
  the threshold passed per call rather than stored. A bake discards parameters,
  and the artist is the one who pays for that, so the engine advises and the host
  asks. The report names the two mechanisms separately, because hPolish and Move
  degrade for genuinely different reasons and a policy keyed on one would miss
  the other.
- **The scope**: a layer. An arbitrary run of siblings has no self-contained
  field — an edit list is ordered and its operators relative — where a layer
  does, since layers combine by hard union.
- **One command**: yes, and no new command was needed. `RemoveNodeCmd`'s inverse
  already carries the removed subtree by value, so a group of removals plus one
  add has exactly the inverse this asks for.
- **Re-expansion**: one-way. What was absorbed is in the undo record, which is
  where going back belongs; a separate un-bake would have to invent parameters
  for a shape that no longer has any.
- **`add-multi-resolution`**: still open, and the shape of the interaction is
  now concrete rather than hypothetical — `ConsolidationParams::cell_size` is
  the resolution a bake fixes, so levels would supply it instead of the caller.

## Impact

`sdf-kernels` gains the statement that a chain's declared Lipschitz is the
signal. `scene-model` gains the command and its inverse. `c-abi` and
`python-bindings` gain the surface and the cost report. Additive: a document
that never consolidates behaves exactly as today, which is also why the
degradation above is a live problem rather than a hypothetical.
