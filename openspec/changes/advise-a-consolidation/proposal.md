## Why

`clay_layer_field_report` already sets `advises_consolidation` — "degraded AND
consolidation is the cure". A host that receives that flag cannot act on it.
The next call it needs is `clay_layer_consolidation_cost` or
`clay_layer_consolidate`, both of which take `clay_consolidation_params`, whose
first field is:

```c
/* Required, and > 0. A document has no intrinsic scale to derive a
 * resolution from the way a mesh's own bounds give one, so guessing here
 * would fix the shape's resolution at a number nobody chose. */
float cell_size;
```

So the engine tells a host it should bake, and then requires it to supply the
one number it has no basis for. The host's realistic options today are a
constant compiled into the app, or a slider the sculptor is asked to set — and
the sculptor cannot be expected to know what "consolidate" means, let alone at
what resolution.

This is ROADMAP **#15, automatic background consolidation**, which was left as a
decision rather than a feature: *"The answer is probably a recommendation the
host can act on with one call, not an autonomous action."* This change settles
it as that recommendation.

## What Changes

**One new entry point.**

```c
clay_result clay_layer_consolidation_advice(const clay_document* doc,
                                            clay_layer_id layer,
                                            float advise_below_step_scale,
                                            clay_consolidation_params* out_params,
                                            clay_consolidation_cost*   out_cost,
                                            int32_t* out_advises);
```

It answers the question `advises_consolidation` leaves open: *if I bake this
layer, at what resolution, and what will it cost me?* The advised params are a
descriptor the caller owns and may edit; feeding them straight into
`clay_layer_consolidate` is one of the things a host may do with them, not the
only one.

`cell_size` is derived from the layer's own extent and the smallest thing the
layer already carries — for a layer holding baked volumes, which is the only
degradation this ever fires on, it returns the finest cell size an earlier bake
already chose. `band` and `padding` are the engine's own defaults spelled out
rather than left as zeros, so a host that stores the advice re-bakes the same
box later. The arithmetic is in `design.md`.

**The advice is keyed on the PROJECTION, not on the flag.** `*out_advises` is 1
only when the field report advises consolidation at the caller's threshold AND
the projected `safe_step_scale` in `out_cost` clears that threshold. A
redistanced volume declares `sqrt(3)` times its samples' Lipschitz, so a baked
layer's step scale is at best `0.577` — a host asking for `0.8` is asking for
something no bake can deliver, and it should be told 0 rather than handed params
that make its layer worse. This is #387's rule (the advice follows the
mechanism) carried one step further: the advice now also follows the *number*.

**When not advised, `out_params` and `out_cost` are ZEROED** past their
`struct_size`. `cell_size == 0` is exactly the value `clay_layer_consolidate`
refuses, so a host that ignores `*out_advises` and passes the params on gets
`CLAY_ERROR_INVALID_ARGUMENT` rather than a bake at a resolution nobody chose.

**Nothing bakes and nothing severs.** Same rule as
`clay_layer_consolidation_cost`: asking what a bake would cost must never be the
thing that unlinks a subtool.

## The prose this has to answer

The header says guessing a `cell_size` "would fix the shape's resolution at a
number nobody chose". That sentence is correct and this change does not overturn
it. Three things separate it from what is proposed:

1. **It is about a DOCUMENT.** A document has no scale; a *layer with content*
   has exactly what the sentence says a mesh has — its own tight bounds,
   answered for all three representations since 0.52.3 (issue #318). The
   analogy in the sentence is the argument for this call.
2. **Nothing is fixed.** `cell_size` stays required and `> 0` on
   `clay_consolidation_params`; no call gains a default, a zero-means-guess
   mode, or a stored resolution. This fills a caller-owned struct that the
   caller then passes, edits or discards. It is advisory in the same sense
   `advises_consolidation` is, and the header already accepted that shape:
   *"The trigger is ADVISORY. Nothing here bakes on its own."*
3. **"Nobody chose" stops being true where the call fires.** Only
   `CLAY_DEGRADATION_VOLUMES` is ever advised, and such a layer carries baked
   volumes whose cell sizes somebody chose at an earlier bake. The derivation
   hands the finest of them back unchanged. Where no volume is present the
   number comes from the smallest feature the artist authored. It never invents
   a constant.

And a fourth guard the original sentence did not have available: the call quotes
the bricks and the bytes at the number it advises, before anything is spent.

## What is NOT changed, and why

**The engine still never bakes on its own.** Consolidation is destructive and
undoable; firing it on a background thread mutates a document behind a host that
may be mid-undo-group or mid-save, and it discards parameters the artist may
still want. A recommendation costs a host one call to ignore.

**The Lipschitz does not set the resolution.** `clay_field_report.lipschitz` is
a STEPPING bound, not a slope bound (`Tape::lipschitz_bounds_gradient`), so it
does not bound how fast the field varies across a cell and cannot be turned into
a sampling rate. It enters the advice — through `degradation` and the projected
step scale — and not the resolution. `design.md` records this, because deriving
a cell size from it would look principled and be unsound.

**No cheap arm.** `out_cost` may be NULL, but the sampling happens anyway,
because `*out_advises` is defined in terms of the projection. The header says so
outright so that nobody reads NULL as a fast path.

## Capabilities

### Modified Capabilities
- `c-abi`: the advisory trigger becomes actionable — one call turns the flag
  into params a host can pass on, with the projected cost beside them.
- `python-bindings`: pyclay follows, or `check_binding_parity.py` fails.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp` — the entry point.
- `include/clay/scene/consolidate.h`, `src/scene/consolidate.cpp` — the
  derivation and the projection, beside `report_layer` and the bake they both
  have to agree with.
- `bindings/python/pyclay_module.cpp`, `bindings/python/` — `Layer.consolidation_advice`.
- `tests/unit/`, `bindings/python/tests/`.
- `docs/05` (library reference).
- **ABI 0.85.0 -> 0.86.0.**
