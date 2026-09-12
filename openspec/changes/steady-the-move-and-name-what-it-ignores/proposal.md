## Why

`clay_move_params` was `{struct_size, radius, ease, front_only}`.
`clay_stroke_preset` — which every other brush family resolves through
`clay_stroke_resolve` — carries twelve stroke controls. **Move reached none of
them**, so a host could not offer lazy-mouse on the one brush where a sculptor
most expects it.

## Do NOT fix this by routing Move through the preset

That was the obvious design and it is wrong. From Blender's own documentation:

> Grab "only moves the vertices that are under the brush radius **at the start
> of the stroke**", and "Pressure Sensitivity is **not supported** for this
> brush type".

Blender anchors the **region** at press and lets only the **displacement**
follow. A grab is **one deformation per stroke, not one per dab**. Honouring
`spacing` — the field that would appear to "work" — is exactly what converts one
gesture into tens of permanent warps. **Parity with ZBrush and Blender is
achieved by NOT resolving dabs.**

Of the twelve controls, exactly one is unambiguously meaningful to a grab.

## One field, and the rest written down

`steady`, same semantics and units as `clay_stroke_preset.steady`, appended
behind `struct_size`.

The other eleven are documented beside it with the reason each is absent,
because **a control that does not act is worse than one that is missing**. The
sentence most worth having is that `accumulation` does **not** govern how
successive Move gestures compose — a host assuming it does will ship a Move that
silently accumulates warps. What governs that is `gesture_id`, and nothing else.

## What steady costs, stated rather than discovered

`clay_sdf_move_update` promises that updates of 0.1, 0.2 then 0.5 end exactly
where a single fresh drag of 0.5 does. **Lazy-mouse lag is path-dependent by
definition**, so at `steady > 0` that promise no longer holds and cannot — the
whole point is that the surface trails the cursor. At `steady == 0` the path is
bit-identical to before the field existed.

## Refused where it could not act

`clay_layer_move_surface` applies a whole drag in one call and keeps nothing
between calls, so it has no previous position to lag from. A non-zero `steady`
there is **refused**, not ignored — which is the same principle the ignore-list
is written for.

## What building it found

**The field was first inserted before `gesture_id` rather than after it.**
`gesture_id` shipped in ABI 0.107.0, so moving its offset would have broken
every caller compiled against that version — the `struct_size` pattern only
works by appending. Caught by reading the field order back after the edit, and
now pinned by a test that drives a descriptor sized to include `gesture_id` but
not `steady`.

## Unblocked by #533

This issue was blocked on the coalescing defect: a pressure-driven radius cost
101x, so no pressure-like control could be offered at all. That is fixed, and
`pressure_size` is still declined here for the separate reason that a grab's
radius is frozen at press.
