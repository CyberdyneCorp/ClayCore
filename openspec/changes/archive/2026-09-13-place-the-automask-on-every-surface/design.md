# Design

## Why a base class and not a member

Four handles carry the frame and every helper takes the frame alone. A member
(`sculptor->session.has_frame`) would name the carrier twice at every call site
and would rename the field on the handle that already had it. Inheriting keeps
`sculptor->has_frame` reading exactly as it did, so the mesh path's diff is the
helper signatures and nothing else — which is what makes it a safe base for the
three handles that had no frame at all.

The handles are all default-constructed (`make_unique<>()` / `new X{}`), so a
base adds no churn at any construction site.

## Why the descriptor moves with the gate, or neither does

Placing the mask gate without converting the brush descriptor would leave a
stamp centre in layer-local coordinates while the gate reads world — a NEW
disagreement of exactly the kind the original comment describes as the defect:

> the calls crossing that boundary did not agree: a raycast took a world ray and
> returned a world hit, and the stamp that hit fed read its centre as local,
> took no frame, and said nothing about it.

So each site gets `brush_settings_to_local` beside `mask_gate_for`. This is also
why the first revert proof was invalid: reverting both at once made the stamp
reach nothing, so the test failed at its precondition and said nothing about the
mask. One revert per property, and the property here is the gate.

## Where this does NOT help

- **A session that declares no frame.** Unset is the identity, so every existing
  host gets exactly the behaviour it had. There is no cost to save and no
  correctness to gain: the surface already was its own world.
- **`clay_multires_sculptor_apply_stroke` with a per-call `mesh_to_world`.** It
  was already correct and is unchanged. The only new behaviour is that passing
  BOTH a per-call frame and a declared one is refused.
- **A per-axis scale.** Refused, not approximated, exactly as on a mesh layer.
  Closing that needs an anisotropic brush footprint, which is its own change.
- **A standalone hierarchy.** `_use_layer_transform` answers
  `CLAY_ERROR_NOT_FOUND` because the hierarchy belongs to no layer. Declaring
  the frame explicitly is the route there.

## What this does not test, said here rather than left to be discovered

`tests/unit/test_c_place_every_surface.cpp` drives the C ABI, so it does cross
the boundary a C++-only fixture cannot. What it does NOT cover is a host's own
carried-gesture logic: a host that converts a gesture into layer space itself
and then ALSO declares a session frame would place the point twice, and nothing
in this ABI can detect that — the frame is a statement by the caller about the
coordinates it is passing. The header says a declared session speaks world in
every call, which is the only available guard.

## The class this belongs to

The defect was found by reading an implementation, not by a failing test, and
every artifact around it was a well-formed answer to a narrower question:

- a comment accurate about the code that asserted a reason which did not hold,
  so a reader who checked it found it **confirmed**;
- an ABI where one entry point required a frame and its own handle claimed to
  have none, three hundred lines apart;
- `moved_vertices`, which cannot distinguish "fully masked" from "reached
  nothing" and would have hidden the whole thing behind an ordinary number.

The general shape is **a measurement that cannot express the failure it is
watching for**. Vagueness invites a second look; a well-formed wrong reason
closes the question. That is why the header sentence is replaced rather than
qualified, and why the test asserts positions rather than the count the ABI
offers for free.
