## Why

The roadmap's Milestone H asks for SDF sculpt layers, and asks one question
before any of it:

> **Hard question: strength.** For geometry represented by CSG/SDF composition,
> multiplying arbitrary field values is not automatically equivalent to scaling a
> displacement. Define what layer strength means before implementation. Do not
> promise one opacity slider over arbitrary CSG and discover later that 0.5
> changes topology unpredictably.

This change answers it, with measurements, and adds nothing to the ABI. It is
the design half of the milestone: the implementation is a separate change and
should not start until this is agreed.

**The audit came back differently than the milestone assumes.** The roadmap lists
eight operations a sculpt layer needs. Six of them already exist on an ordinary
SDF document layer:

| operation | today |
|---|---|
| create | `clay_add_sdf_layer` |
| rename | `clay_document_set_layer_name` |
| enable/disable | `clay_document_set_layer_visible` |
| reorder | `clay_document_move_layer` |
| undo/redo | the session history, which keys every step by `scene::LayerId` |
| serialize | `.clayspace`, per layer |
| **merge/bake down** | **missing** |
| **opacity/strength** | **missing** |

And since ABI 0.86.0 a layer folds into the layers beneath it with its own
operator — `clay_document_set_layer_composition` takes an op, a blend and a
rounding. So "this pass carves what is under it" and "this pass blends into what
is under it" are already expressible, per layer, and were expressible before
this milestone was written.

So the milestone is not "build a sculpt layer stack for SDF". It is: **an SDF
document layer is already a sculpt layer in six of eight respects, and the two
missing operations are merge-down and the one the roadmap flags as hard.**

## What Measuring Refuted

**The obvious implementation of strength does not work, and the obvious test of
it does not show that.**

The natural reading of "this layer at strength s" is a lerp of the composed
field between the document without the layer and the document with it:

```
d_s(p) = (1 - s) * d_base(p) + s * d_full(p)
```

**First measurement — the surface along a probe ray. Exactly linear, five for
five.** Union, subtract, smooth-blended add, intersect, and a pass mixing add and
subtract: the surface crossing moved to exactly `s` of its full travel at every
stop, worst departure `0.000` in all five.

That result is worthless and the reason is worth stating, because it is the
error this proposal exists to avoid making twice. **The probe could not have
reported anything else.** The ray was normal-incident, both fields have unit
gradient along it, so `(1-s)·d_b + s·d_f` crosses zero at exactly `s` of the way
between the two crossings by construction. Five zeros from an instrument that
cannot produce a non-zero is not five confirmations.

**Second measurement — occupied volume, with a control built to be non-linear so
the instrument is shown able to say no.** The control read `0.187`, so it can.

| pass | worst departure from a linear slider |
|---|---|
| union — a bar laid on the sphere | **0.329** |
| subtract — a channel carved into it | 0.136 |
| smooth add — a blended bump | 0.106 |

And the union, in full, is what an artist would actually feel:

| slider at | effect delivered |
|---|---|
| 0.25 | 0.05 |
| 0.50 | **0.17** |
| 0.75 | 0.51 |
| 1.00 | 1.00 |

**Stable under refinement**, so it is not the grid stair-stepping — `s = 0.5`
reads 0.156, 0.171, 0.178, 0.174, 0.183 at n = 48, 72, 96, 128, 160.

Drag the slider to half and you get a sixth of the pass. To an artist that reads
as a control that does nothing until about 60% and then happens all at once.

**Both measurements are true, and together they say what is wrong.** A field lerp
moves the surface linearly WHERE THE TWO FIELDS ALREADY AGREE IN SIGN, and
admits new material abruptly where they do not. So a lerp is not one slider with
a defect; it is two different operations wearing one control — a linear move on
existing surface, and a threshold on new surface.

## What Changes

**Nothing in the ABI. This change is the definition and the evidence.**

**Strength SHALL NOT be a lerp of the composed field.** Measured above, on the
simplest possible pass.

**Strength is defined on a quantity whose half is defined.** That is what the
other two stacks already do, and neither of them generalises to the third:

- `mesh::SculptLayerStack` stores displacement COEFFICIENTS and composes
  `E(n) = B(n) + Σ sᵢ · mᵢ(v) · Lᵢ(n, v)`. Half a displacement is half as far.
- `voxel::VoxelGrid` stores a per-cell DIFFERENCE and defines partial strength as
  a reproducible dithered fraction of CELLS, against the same cell-coordinate
  hash the falloff brushes use. Exact at 0 and 1.

Two representations, two definitions, each in terms of what it stores. An SDF
layer stores declarative nodes, and the honest consequence is that **strength
over arbitrary CSG has no definition in terms of what it stores** — which is why
this proposal does not invent one.

**So the recommendation is that an SDF layer's strength is offered only where it
is defined, and refused where it is not**, rather than offered everywhere and
approximate somewhere. Two places it is defined:

1. **A deformation pass.** A bend, twist, taper or lattice has an amplitude, and
   scaling an amplitude is exactly what a strength slider means. This is the
   roadmap's strategy 2 and it needs no new representation.
2. **A baked detail pass.** Sample the layer's contribution as a displacement
   from the captured base and it becomes the mesh model, where strength already
   has a definition and a shipped implementation. This is strategy 3, and it
   costs the layer's declarative re-editability — which is a real cost and should
   be the artist's explicit choice rather than a silent conversion.

**A pass of authored CSG nodes gets `enable/disable`, not a slider.** That is the
whole finding: it is better to ship a control that means something for two kinds
of pass and a checkbox for the third than one slider that is a threshold in
disguise.

## What This Does Not Decide

Merge/bake down for SDF layers. It is the other missing operation, both other
stacks have it (`clay_multires_merge_sculpt_layer_down`,
`clay_voxel_merge_sculpt_layer_down`), and it is not blocked on the strength
question. It should be its own change.

## Verification

- The probes are `strength_probe2.py` (ray, five ops) and `strength_probe3.py`
  (volume, with the control), both reproducible against a built pyclay.
- The refinement sweep is in the proposal above and converges.
- No code, no ABI, no version bump.
