# Proposal: scale an item per axis

## Why

Issue #320. Every transform in the interface takes a single `float scale`:

```c
clay_layer_set_transform(doc, layer, node, position, axis, angle, float scale);
clay_document_set_layer_transform(doc, layer, position, axis, angle, float scale);
clay_mesh_transform(mesh, position, axis, angle, float scale, &out);
clay_item_set_scale(item, float scale);   /* "uniform, > 0" */
```

The shapes a hard-surface boolean workflow cuts with are mostly NOT uniform. A
slot is a squashed capsule. An oval bolt hole is a squashed cylinder. A chamfer
along one edge is a box stretched on one axis. The primitives that carry their
own extents — `CLAY_PRIM_BOX`, `CLAY_PRIM_ELLIPSOID` — can say it at creation
and never afterwards, so an artist who places a cylinder cannot make it an
oval: they delete it and place a differently-parameterised one, losing where it
stood, or reach for a lattice deformer, whose interaction with the item's own
boolean is a separate question.

The manipulator shipped on placed objects offers ONE scale handle because three
axis boxes would be three controls for one number. The deformation cage keeps
all three, because it moves its own control points and carries no engine
transform. That split is honest and it is not what anyone reaching for a scale
gizmo expects.

## The engine already has the answer, and it is not the one the issue braced for

#320 offers an out: *"if the honest answer is 'the field cannot carry this and
the deformer is where it belongs', saying so in the header would itself be
worth having"*. That is not the honest answer.

`include/clay/kernel/xform.h` has carried the operator since before this issue
was filed:

```c
CLAY_FN cfloat3 cscale_nu_point(cfloat3 p, cfloat3 s) { return p / s; }
CLAY_FN float cscale_nu_dist(float d, cfloat3 s) { return d * cmin(s.x, cmin(s.y, s.z)); }
```

and `exactness.h` classifies it: `cfi_scale_nonuniform` **loses exactness and
keeps the Lipschitz bound at 1**. The issue's fear — "the gradient is wrong by
the ratio of the axes, and every marcher and every Lipschitz bound downstream
inherits that" — is exactly right about the naive `d(p/s)` and exactly what the
conservative multiply is for. Dividing by `s` and multiplying back by the
SMALLEST component never overestimates: `|grad| <= min(s) * max(1/s) = 1`. Every
marcher stays safe at full steps; what is lost is that the value stops being a
true Euclidean distance, which is a thing this engine already tracks per node
and already reports through `clay_layer_safe_step_scale`.

Nothing in the tape, the scene or either binding uses any of it. One kernel-level
test (`tests/unit/test_xform_repeat.cpp`) is the only caller in the repository.

**And the tape record already has the shape.** A primitive's parameter block is
`[inv affine 12] [scale s] [rounding r] [colour] [params] ...`, where the
inverse matrix already contains `1/s` and the interpreter multiplies the local
distance back by `s`. A per-axis scale folds into that with **no new opcode and
no wider record**: put `S^-1` into the inverse matrix, and put `min(sx, sy, sz)`
in the scale slot. `v.d = prim_value * scale - round` then IS `cscale_nu_dist`.

So this change is plumbing over an operator the engine already owns and has
already reasoned about, which is a much smaller thing than #320 assumed.

## Approach

A per-axis scale on a placed node, applied INNERMOST in the item's own local
frame, multiplying the uniform scale the transform already carries:

    world = layer.xform * item.xform * diag(scale_axes)

It is a field on the node beside `rounding`, NOT a widening of
`math::Transform`. That matters: `math::Transform` is a similarity — rotation,
translation, one scale — and its algebra (`operator*`, `inverse`) is closed
because of it. A non-uniform scale does not commute with rotation, so widening
`Transform` would turn every composition in the engine into a general matrix and
take the exactness bookkeeping with it. Keeping the per-axis scale innermost and
node-local means every existing composition is untouched and the only places
that change are the four that build a matrix from an item.

A node whose per-axis scale is uniform — including the default `(1, 1, 1)` —
compiles to bit-identical tape and keeps `is_exact`, so no existing document
changes in any way.

## What is NOT in this change, and why

**A layer's per-axis scale.** `clay_document_set_layer_transform` keeps its
single factor. The layer arm is not more of the same work: `layer.xform * node.xform`
is consumed as a rigid *frame* by `brush::move` and `brush::lattice_gizmo` — they
place a manipulator and a cage in it — and a frame with a per-axis scale is not a
`math::Transform` at all. Deciding what those two should do with one is a
question #320 does not settle and the item arm does not need. Named here rather
than left for someone to discover.

**A mesh's per-axis scale.** `clay_mesh_transform` is real vertices and no
field, so it is a genuinely separate and much simpler piece — positions by the
matrix, normals by its inverse transpose. It is in scope and is the one arm that
touches no SDF machinery at all.

## Impact

`scene-model` gains the per-axis scale and its exactness consequence.
`file-io` takes a format minor: the node record grows three floats, appended
last and gated, so a build that predates it reads exactly the bytes it always
did and writing AT an older minor degrades a squashed cylinder to a round one
rather than to a missing one. `c-abi` gains the setters, the readers and the
mesh transform. `python-bindings` gain the same surface so the parity gate stays
clean.

Scene and `.clayspace` minor 14; ABI 0.54.0.
