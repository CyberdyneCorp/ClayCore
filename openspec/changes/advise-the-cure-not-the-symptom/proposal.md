# Key the consolidation advice on the mechanism, not the symptom

## Why

`clay_field_report` names the two degradation mechanisms separately, and
`consolidate.h` is explicit about why: "a policy keyed on only one of them would
miss the other." But `advises_consolidation` was `safe_step_scale <
advise_below_step_scale` and nothing else — and a grab chain lowers the step
scale exactly as a stack of baked volumes does. So the advisory fired hardest on
the case consolidation cannot help.

Measured by the reporting host (issue #387), on a real form, medians over four
gestures with the cold one discarded:

| brush | before consolidating | after | |
|---|---|---|---|
| Polir (bakes, held whole) | 2647 ms | 202 ms | **13x better** |
| Suavizar (`clay_sdf_smooth_*`) | 595 ms | 947 ms | 1.5x worse |
| Mover (`clay_sdf_move_*`) | 211 ms | 1345 ms | **6x worse** |

The Move row is the striking one, because the collapse IMPROVES the number that
triggered it: the step scale goes from 0.00275 to 0.08090, a 29x win, and the
gesture gets 6x slower. Nothing was in the chain to win back — the drag had
already collapsed to one grab per gesture, and the layer was one analytic item —
so the bake only swapped a cheap primitive for a warped 3.3 MB volume.

## What Changes

- `advises_consolidation` is true only when consolidation is the cure for what
  is actually wrong: when there is an EDIT LIST to absorb or STACKED VOLUMES to
  redistance. Those are the only two things a bake wins back.
- A `degradation` field names the mechanism outright — none, volumes, deformers,
  both — so a host can pick a cure rather than reading one out of a boolean.
- `steepest_deformer_chain` reports the deformer mechanism's own FACTOR.
  `longest_deformer_chain` is a COUNT and cannot be weighed against
  `steepest_volume`: a chain of four gentle warps and a chain of one deep grab
  are the same length and cost the marcher nothing alike.
- `drawable_count` reports how many of `item_count` actually contribute a field.
  A group is a transform and a name and is never evaluated, so it is not an edit
  list to win back — keyed on `item_count`, a lone item wrapped in a group would
  get the advice that measured 6x worse.

## What this does NOT claim

There is no new cure offered for the deformer mechanism. When `degradation` is
`Deformers` the honest answer is that the layer is already parametric and cheap
per sample and it is the marching that costs; #386 makes that case much rarer by
no longer charging disjoint brushes for a compounding that cannot happen.

## Impact

- Affected specs: `sdf-kernels`, `c-abi`, `python-bindings`
- Affected code: `include/clay/scene/consolidate.h`, `src/scene/consolidate.cpp`,
  `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `bindings/python/pyclay_module.cpp`
- **Behaviour change for a host reading `advises_consolidation`**: it stops
  firing on a layer degraded only by its brush chain. That is the point. A host
  that wants the old meaning has `safe_step_scale` and its own threshold.
- Additive at the ABI: three fields appended to `clay_field_report` behind
  `struct_size`, so a caller built against the older struct is unaffected and
  nothing is written past the end of the struct it owns.
