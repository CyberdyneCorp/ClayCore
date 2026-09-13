# Place the brush's lattices on every surface, not only on a mesh layer

## Why

A layer's vertex arrays are LAYER-LOCAL and its `xform` places them. The
lattices a brush consults — the painted mask, the cavity field, the group field
— are WORLD-ADDRESSED. Something has to place the point between the two, and
until now only `clay_mesh_sculptor` could: `define-carried-mesh-transform-
semantics` gave it a session frame and left the other three handles carrying
nothing.

So on a placed layer the multires sculptor, the dynamic sculptor and the
sculpt-layer stroke sampled every lattice at the **unplaced** point and gated
the wrong region. It presents to an artist as "the mask didn't take", which is
why it went unfiled through several releases.

`reach-every-automask-from-a-host` (ABI 0.96.0) then made
`CLAY_AUTOMASK_CAVITY` and `CLAY_AUTOMASK_SURFACE_GROUP` reachable from C on
all three sculptors, and passed no frame on two of them because there was none
to pass. That is where the defect was noticed — but it is NOT where the defect
is. `clay_multires_sculptor_stamp` and `clay_dynamic_sculptor_stamp` have taken
a `mask` and sampled it unplaced at every version that had them, so a host that
never touches `clay_automask_sources` is affected too.

### The proof this is layer-local and not incidentally untransformed

`clay_multires_sculptor_apply_stroke` was already correct. It takes a per-call
`mesh_to_world` and places every lattice through it, and its header says so:
"it places each vertex onto every world-addressed lattice this call samples —
the painted mask, the cavity measure and the group field."

So one entry point on the multires sculptor required a frame while the handle
carried none, three hundred lines apart. That settles the question from inside
the ABI rather than from a host's evidence, and it is corroborated by storage:
`clay_document_mesh_layer_by_id` hands back the layer's mesh untransformed and
`clay_multires_from_mesh` copies that cage, so a hierarchy built from a placed
layer inherits layer-local positions and nothing downstream places them.

### The comment that was accurate about the code and wrong about the geometry

`read_automask_sources` recorded the two null frames as deliberate: "Null for
the two sculptors that declare no frame, where the identity is the truth rather
than a default." Accurate about the code, and the expensive kind of wrong,
because a reader who checked it against `clay_c.cpp` found it **confirmed**. The
identity was never the truth for a hierarchy built from a placed layer; it was
the only thing expressible. That sentence is replaced rather than supplemented.

## What changes

- One `SessionFrame` carrier — `has_frame` plus a `math::Transform` — shared by
  all four handles instead of owned by one, with the seven existing helpers
  retargeted at it. No behaviour change on the mesh path, which is what makes
  it a safe base for the three that had none.
- `_set_world_frame` and `_world_frame` on the multires sculptor, the dynamic
  sculptor and the sculpt-layer stroke; `_use_layer_transform` on the two whose
  surface can be borrowed from a document layer.
- The painted mask gate placed at all three defect sites, one of which serves
  five stroke verbs through a single helper.
- The brush descriptor's world-valued fields converted on those paths. BOTH
  HALVES OR NEITHER: a placed mask gate beside a local stamp centre would be a
  new disagreement of exactly the kind this frame exists to remove.
- `reject_conflicting_frame` extended to
  `clay_multires_sculptor_apply_stroke`, the one call that can now spell the
  frame twice.

## What does not change

UNSET IS THE IDENTITY on every one of these, so a host that has not heard of
them is not opted in and nothing it already does behaves differently. The
per-call `mesh_to_world` on `_apply_stroke` keeps working exactly as before for
a session that declares no frame.

A per-axis scale is refused rather than approximated, for the reason
`clay_mesh_sculptor_use_layer_transform` already gives: under one, a round brush
in world is an ellipsoid on the model, so `radius` stops naming anything a
spherical walk can honour. The two must not answer differently.

The dynamic surface gets no `_use_layer_transform`, because a
`clay_dynamic_surface` is not a document layer and there is no transform to
read. That is stated in the header as a fact about the ABI rather than left as
an apparent omission.

## Bound

`grep` for the unplaced form across `clay_c.cpp` returns exactly four sites: the
three fixed here, plus the `!has_frame` identity fallback inside `mask_gate_for`
which is the correct one. The blast radius is a sweep, not an enumeration.
