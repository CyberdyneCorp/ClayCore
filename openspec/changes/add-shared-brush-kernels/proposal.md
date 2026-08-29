# Proposal: shared brush kernels

## Why

Three sculptors are coming — the fixed-topology one that exists, an adaptive
one, and a multiresolution one — and every specification for the other two says
the same thing: reuse the brush math rather than copying it. Today that math is
not reusable. It lives inside `src/mesh/sculpt.cpp` interleaved with the
representation plumbing that feeds it: the weld-class walk, the region gather,
the write-back, the local normal recompute. A second caller can only have the
deformation by having the plumbing too.

The consequence if it is copied instead is specific and it is the one thing a
sculpting engine cannot afford: **Clay would stop meaning the same thing on two
representations.** An artist who learns a brush on a mesh layer and finds it
behaves differently on an adaptive one has not found a bug they can report —
they have found that the tool is untrustworthy.

There is a second reason to do this first, and it is about the gate rather than
the design. Extracting the kernels has an exact acceptance criterion while
there is exactly one caller: the fixed-mesh results must not move by a bit.
Once two more callers exist, "did the refactor change anything" stops being a
question a test can answer cheaply.

The third reason is that the brush model itself is thinner than the vocabulary
suggests. `MeshBrushSettings` has grown fourteen fields, several of which are
one verb's business (`polish_angle`, `layer_height`), and the artist-facing
brush families other engines ship — ClayBuildup, DamStandard, hPolish, Trim
Dynamic, Rake — are not new deformations. They are a kernel plus a falloff plus
a frame plus an accumulation rule plus a spacing. Naming those axes separately
is what turns a brush into a preset instead of an engine path, and it is what
lets the next brush cost a serialized struct rather than a code path.

## What changes

- **Representation-neutral kernels.** The deformation math moves to
  `mesh/sculpt_kernels.*` behind a snapshot-in, displacement-out interface that
  names no `Mesh`, no `Adjacency` and no vertex index — a span of positions,
  normals and weights, a neighbourhood view, and the stamp's frame.
- **An orthogonal brush model.** `brush::BrushModel` names the axes a
  professional brush composes from — footprint, weight, frame, kernel,
  accumulation, write target, post policy — as enums and PODs, not virtuals.
- **A compiled runtime plan.** The preset is validated and compiled once per
  stroke into a flat `BrushRuntimePlan` the hot loop reads, so a stamp does not
  re-inspect a large settings struct per vertex.
- **A workset and a scratch arena.** One reusable local region — read halo and
  write region distinguished — whose capacity tracks the largest recent
  footprint rather than the model.
- **Automasking over the workset**, composed into the weight rather than
  branched into each verb, and computed from the estimators procedural masks
  already use.
- **A versioned `BrushPreset`** with a reference library covering the artist
  families, serialized without image data.

## Approach

Extract, then generalize, in that order, and gate the extraction on bit
parity. The existing verbs keep their names, their settings and their results;
`MeshSculptor` becomes the fixed-topology caller of a shared kernel rather than
the owner of the only copy.

Automasking reuses `brush::procedural_mask`'s estimators evaluated over the
workset, not a second curvature implementation — a cavity mask a user paints
and a cavity automask a brush applies must not disagree about the same surface.

Frames are named rather than implied. Draw takes the region normal, Inflate the
vertex normal, a rake the stylus azimuth, a trim a given plane; today those
conventions are scattered across the verbs and the difference between Draw and
Inflate is documented as the distinction between two verbs rather than as one
axis with two values.

## Open questions

- **Where the scratch arena lives.** `include/clay/memory/` is a new module and
  `tools/check_layering.py` has no entry for it; `parallel` is the precedent
  for a leaf utility module that was previously trapped inside a backend.
- **Whether `MeshBrushSettings` is deprecated or kept as sugar.** Every shipped
  host and both bindings pass it. Keeping it as a projection onto `BrushModel`
  costs a conversion per stroke and no compatibility.
- **How much of the model the C ABI takes now.** A descriptor mirroring every
  axis is large and is the kind of surface that is expensive to get wrong; the
  minimum is the preset plus the axes the mesh path already exposes.

## Impact

`brush-engine` gains the model, the plan and the preset. `meshing` gains the
shared-kernel requirement and the allocation discipline. `c-abi` and
`python-bindings` gain the preset surface. No behaviour changes on any existing
path, which is the acceptance criterion rather than a hope: the fixed-mesh
golden fixtures compare bit for bit, not within a tolerance.
