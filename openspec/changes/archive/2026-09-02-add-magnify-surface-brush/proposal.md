# Resolve a magnify against the assembled surface

## Why

`clay_layer_move_surface` exists because `grab` is per item and local: a grab
drags one item's own field, so on a form smooth-unioned from several pieces it
pulls one piece's share and leaves the rest behind. The brush doc measures it —
two balls at x = ±0.45 blended at k = 0.25, a grab of radius 0.8 and
displacement 0.4 on the left item: **the left side rises 0.070 and the right
0.000**. Nothing errors. It just comes out wrong.

`CLAY_DEFORM_MAGNIFY` has exactly the same shape and had no such counterpart:

```
grab      per item, local   ->  clay_layer_move_surface      (assembled surface)
magnify   per item, local   ->  (nothing)
```

The deformer docs list the two together twice, in the same sentences, so the
hazard the Move resolver was built for applies to the radial scale verbatim.

What it cost a host (issue #391): `docs/07-brushes-and-features.md` maps Pinch
to `magnify` negative and Magnify to `magnify` positive on the SDF side, and
there was no way to reach either as a SURFACE brush. That left three options and
none of them good — scale one picked item and call it Pinch (the exact failure
the Move note exists to prevent); rebuild the resolver host-side out of
`clay_layer_add_deformer`, which is field math in a host and would have to be
re-derived by every host; or do not offer Pinch on SDF layers at all, which is
what the reporting host does today.

## What Changes

- **ADDED** `brush::magnify_brush`, with `prepare_magnify` and
  `resolve_prepared_magnify` for a live gesture, on `move_brush`'s contract.
- **ADDED** `clay_layer_magnify_surface` and `clay_layer_magnify_surface_preview`
  with `clay_magnify_params`.
- **ADDED** `Layer.magnify_surface` and `Layer.magnify_surface_preview`.
- The chain rule generalises: `moved_chain` recognises a gesture in progress by
  KIND as well as by ball, so a pinch replaces its own last frame and leaves a
  drag's grab over the same spot alone. One rule, one place.
- The gesture's apply half — the reach it invalidates, the frontier it can
  state, the one undo group — is factored out of `clay_layer_move_surface` and
  shared, rather than copied into a second hundred-line body that would become a
  second place to fix #360 and #363.

Not generalised to a deform KIND. `pose` sits in the same sentence and has the
same per-item gap, but radial pose carries an AXIS — a direction that would have
to be mapped per symmetry image — and pose along a line has no finite support at
all, so the reachability rule both resolvers are built on does not apply to it.
A generic `clay_layer_deform_surface` would be a promise about pose that this
does not keep.

## Impact

- Affected specs: `brush-engine`, `c-abi`, `python-bindings`
- Affected code: `include/clay/brush/magnify.h`, `src/brush/magnify.cpp`,
  `include/clay/brush/move.h`, `src/brush/move.cpp`, `bindings/c/clay.h`,
  `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`
- Additive at the ABI: no existing signature or struct changes.
