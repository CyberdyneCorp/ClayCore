# Proposal: the two brushes a stroke cannot spell

## Why

`brush/stroke.h` turns pointer samples into stamps and stamps into ordinary
nodes, and that is the right shape for every declarative SDF brush: a dab IS a
persistent node, so undo, picking, serialization and the C ABI already
understand the whole gesture and the document is never in a state the format
cannot describe.

Two verbs cannot be spelled that way, and both are core sculpting. Each fails
differently, and all three failures are the same missing thing — a place to
keep transient state for the length of one gesture.

**P0-1: Smooth had no live preview, because relax BAKES and there was nowhere
to put the volume.** `field/relax.h` says so in its first paragraph: there is
no node that means "the average of what was here", so smoothing samples the
layer, averages, and hands back a volume rather than an edit list. A host with
nowhere to keep that volume between pointer events has exactly one
implementation per dab — sample the whole layer, relax, throw it away — which
costs the MODEL per dab rather than the brush. The only affordable version of
that is to run it once at pointer-up, so the artist smooths blind and finds out
what happened when they lift the pen. The brush landed
(`add-sdf-relax`, `make-the-relax-dab-local`) and the *interaction* did not.

**P0-2: repeated SDF sculpting degrades the marcher without bound, and nothing
was allowed to stop it.** Every Move stroke prepends a grab, and each grab
multiplies into the declared Lipschitz; every resampled volume decays it again.
`add-consolidation-policy` measured this — `scene::report_layer` reports
`safe_step_scale`, `longest_deformer_chain` and `item_count` — and deliberately
never bakes, because a bake discards the parameters of everything it absorbs
and a library that did that unasked would be deciding on an artist's behalf
that a sphere's radius is no longer editable. That contract is right and is
unchanged here. What was missing is the OTHER half: a place for a session to
say "in this sculpt mode, on this device, with this frame budget, collapsing is
acceptable". Without it the measurement has no consumer inside the library, the
tenth stroke marches at a fraction of the first's step, and the only cure is a
host reaching for `consolidate_layer` with numbers nobody chose.

**P0-3: Move churned the persistent document once per pointer event.** A drag
issues one `SetDeformersCmd` per node it reaches, and a live drag did that per
FRAME: revisions, tapes, brick seeds and picking all rebuilt sixty times a
second to produce one edit. `add-move-drag-continuity` made `moved_chain`
replace a leading grab from the same drag rather than stack another, which
stopped the CHAIN growing per frame and left the churn exactly where it was.
And each of those frames re-walked the whole edit list to rediscover which
items the drag reaches — an answer that cannot have changed, because a drag
holds its centre and its radius fixed and grows only its displacement. Measured
as a counter rather than a clock: over a layer of 5,002 items, preparing the
drag visits 5,002 nodes and every frame after it visits the 2 the drag actually
moves.

## What Changes

**A transaction, in `session/`.** Four verbs, and between the first and the
third the `scene::Document` is untouched — no nodes, no deformers, no undo
entries, and a serialization taken mid-gesture is byte-for-byte the one taken
before it:

    begin    capture the source identity; build the transient state ONCE
    update   mutate only the transient state; report what went dirty
    commit   one persistent command group; then, optionally, one policy
             consolidation inside the same group
    cancel   nothing persistent ever happened

`SdfSmoothTransaction` samples the layer once at pointer-down into a working
volume it owns, relaxes that volume locally per dab, and installs it as the
layer's single item at pointer-up **without sampling again** — so a commit
installs the bytes the artist was looking at, not a second bake that is merely
close to them.

`SdfMoveTransaction` finds the affected items and their frames once at
pointer-down, rebuilds the preview each frame from the immutable pre-stroke
chains plus one grab for the CURRENT TOTAL displacement, and writes every final
chain as one undo step at pointer-up. The preview is a private copy of the
`scene::Layer` with the affected chains replaced, so a host compiles, draws and
picks it through the paths it already has.

**Five smaller pieces, each of which is the half of an existing thing that was
never separable.**

- `field::relax_in_place` — the same arithmetic as `relax` with different
  ownership. `relax` copies its input, which is right for a standalone
  operation and wrong for a live gesture that already owns its working volume.
  It returns a `RelaxResult` — dirty bounds, touched bricks, changed,
  cancelled — so a host invalidates a region rather than a model.
- `FieldVolume::rewrite_region_tallied` — what a region rewrite actually wrote.
  A host holding a preview cannot SEE a rewrite: the volume keeps its identity,
  its brick set and its bounds, so the only safe answer without this is "all of
  it", which is the term scaling with the model that a local dab exists to
  remove.
- `scene::replace_layer_with_volume` — the install half of `consolidate_layer`,
  shared rather than duplicated. Consolidation is two things sold together;
  a Smooth stroke has already done the first half and must not be made to do it
  twice to reach the second.
- `brush::prepare_move` / `resolve_prepared_move` — the half of a drag that
  does not depend on how far it has gone, split from the half that does.
  `move_brush` is now prepare-then-resolve, so there is one geometry and not
  two. Plus a `moved_chain(chain, warp)` overload for a caller holding the
  pre-stroke chain by value, which a live drag must.
- `UndoStack` grouping becomes NESTABLE — `bool grouping_` is now an `int`
  depth, and nested brackets collapse into the outermost step. An inner bracket
  used to open a second step, so an entry point that groups its own work could
  not be called from inside a caller's group without splitting one gesture into
  two undos.

**A session policy that is never the engine's decision.**
`SdfSculptComplexityPolicy` carries the three criteria `report_layer` already
measures separately, plus an `allow_consolidation` that is **off by default**.
Over budget with it false is a REPORT: the transaction says so and changes
nothing. With it true the collapse happens inside the stroke's own undo step,
so the artist undoes one thing having done one thing.

## Capabilities

### New Capabilities

- `sdf-sculpt-transaction`: the gesture lifetime a FIELD or DEFORMATION brush
  needs and an edit-list brush does not — what a host may hold open, what the
  document is guaranteed not to do while it is open, what one update reports,
  what a commit installs, and when a session may spend parametric history to
  keep the marcher affordable.

### Modified Capabilities

- `sdf-kernels`: relax gains an in-place form whose cancellation contract is
  whole passes rather than "hand back the input", and a region rewrite reports
  the bricks it selected and whether any sample moved.
- `scene-model`: undo brackets nest, and an already-computed volume can be
  installed as a layer's single item with everything the collapsed
  consolidation guarantees.
- `brush-engine`: a drag resolves in two halves — the traversal, paid once, and
  the per-frame warp, which costs the items the drag moves.

## Impact

**Additive, and C++-only for now.** New headers `clay/session/sdf_sculpt.h` and
new entry points on `field`, `brush` and `scene`; no existing signature
changes, no format change, no ABI change, no version bump for the on-disk
layout. `relax()` still returns its INPUT on cancel, `move_brush` still returns
the warps it always did, `rewrite_region` is `rewrite_region_tallied` with the
report dropped so there is one walk and not two, and `consolidate_layer` is
`bake_layer` followed by `replace_layer_with_volume`.

`src/session/sdf_sculpt.cpp` joins the library in `CMakeLists.txt` and
`tests/unit/test_sdf_sculpt.cpp` joins `tests/CMakeLists.txt`. `session/` sits
above `scene`, `brush` and `field` and below nothing, which is the layering
`check_layering.py` already enforces.

**Not reachable from a host that is not C++.** The C ABI, the Python bindings
and the docs are tasks in this change and are not done: an opaque handle with
begin/update/commit/cancel is the obvious shape and it is deliberately not
guessed at here. See `tasks.md` §17.

## Non-goals

**A local working patch for Smooth.** The working set is the whole finite
layer, and that is a decision rather than an oversight — see `design.md`. Lazy
local checkpoints are the follow-up, and `begin()` is the thing to benchmark
before building them.

**A transaction for the edit-list brushes.** Clay, Inflate, Pinch and the rest
already have the right lifetime: a dab is a node, and there is nothing
transient to hold. Wrapping them in this would be ceremony around a thing that
already works.

**A document that can hold an open gesture.** A `.clayspace` describing a
half-finished Smooth is a state a loader would have to decide what to do with.
Nothing here is serializable and nothing here should become serializable.

**A cross-layer or cross-document gesture.** A transaction owns one layer,
stamped at begin. Smoothing two subtools at once is two transactions, and the
fingerprint check makes their interleaving safe rather than silent.
