## Why

`field::flatten` has two overloads and the C ABI exposes one of them. The
header says which one a caller should want, in as many words:

> The same, with a volume as the source — which is what an imported mesh gives,
> since there is no document behind it. Accurate while the surface stays near
> the band it came from: past that the source reports a bound rather than a
> distance, so the facet is placed against a lower bound on where the surface
> was rather than against the surface. **Where a document exists, sample from
> that instead.**

There is no way to sample from a document through the C ABI. `pyclay` has
both — `Volume.flattened` for a volume and `Volume.flattened_from` for a
source plus its own `cell`, `band` and `bounds` — and the iPad app, which is
C, can only reach the first.

`tools/check_binding_parity.py` maps both Python names to
`clay_item_volume_flatten`, so the gap passes the parity gate. Two names
pointing at one symbol satisfies the table; nothing in it can say "these are
different operations and only one is exposed".

**What the difference actually is, measured.** An earlier draft of this
proposal claimed the volume-sourced path places the facet wrongly past the
band, from a gallery render that came apart. Measuring it disproved that: on a
bumpy ball at `cell 0.015`, both paths put the facet at y=0.460 and enclose
the same volume, at bands of 0.045, 0.090 and 0.120 alike. **The surface is
the same.**

What differs is STEEPNESS, and by a lot:

| band | facet (volume / document) | `safe_step_scale` (volume / document) |
|---|---|---|
| 0.045 | 0.460 / 0.460 | **0.0427 / 0.3539** |
| 0.090 | 0.460 / 0.460 | 0.0451 / 0.3539 |
| 0.120 | 0.460 / 0.460 | 0.0421 / 0.3539 |

Flattening a volume blends the plane with a source that is itself sampled, and
the result declares a Lipschitz roughly **8x worse** than flattening from the
exact document — independent of the band, so this is not a band effect either.
The engine's own raycast marches by `safe_step_scale`, so that is 8x the
marching cost for the same shape, and past some point the marcher runs out of
iterations and the surface stops being drawable at all. That is what the
gallery render was showing, and it is the same failure shape as the Move
degradation: a correct surface in a field too steep to trace.

## What Changes

- **`clay_item_volume_flatten_from`** — flatten sampled from a document,
  taking the flatten parameters, the sampling parameters (`cell_size`, `band`,
  `padding`) and an optional region, and returning a new volume item. The
  sampling half mirrors `clay_item_volume_from_document`; the flatten half
  mirrors `clay_item_volume_flatten`, so neither validation rule is restated
  in a second place with a chance to drift.
- **The parity table stops hiding the asymmetry.** `Volume.flattened_from`
  maps to the new symbol rather than sharing one with `Volume.flattened`.
- `clay_item_volume_flatten` is **unchanged**. It is the right call when the
  source is a volume and no document exists — an imported mesh — and its
  header already states the accuracy limit that makes it the second choice
  otherwise.

## Capabilities

### New Capabilities

(none — this adds an entry point to an existing capability)

### Modified Capabilities

- `c-abi`: a document-sourced flatten is reachable from C, so the sound path
  is not Python-only.

## Impact

- **ABI**: one added symbol. Additive — no signature changes, nothing removed,
  no struct grown. Version moves to 0.27.0, unreleased.
- **Code**: `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `tools/check_binding_parity.py`.
- **Tests**: a C ABI test that the document-sourced result differs from the
  volume-sourced one where the band is the limit — the defect this closes, not
  merely that the call returns `CLAY_OK`.
- **No engine change.** `field::flatten` already has the overload; this is ABI
  surface over it.
