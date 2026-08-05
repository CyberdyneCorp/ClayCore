# Proposal: pose with a line-gradient region

## Why

`add-region-deformers` shipped pose with **radial (sphere) weights only**. The
study's draft requirement names three region sources — line gradient, sphere,
and a mask reference — and that change deferred the mask explicitly while saying
nothing either way about the line. So pose currently covers one of three, and
the one it covers is not the one the tool defaults to.

The line gradient is how Pose is actually used: anchor at a joint, drag to the
tip, and the rotation ramps along the limb. Every hard-surface taper workflow in
the corpus leans on it. Radial weighting cannot express it — a sphere centred on
a joint rotates the material *behind* the joint as much as the material in front.

## What it is not

Unlike grab and radial pose, a line gradient **does not have finite support**.
The weight clamps: past the end anchor everything is fully rotated, not
untouched. That is the point of the tool — the whole limb tip moves — but it
means this deformer cannot claim the locality property its siblings have, and
its bound has to account for a swept arc rather than a dilation. The spec says so
rather than leaving the difference to be discovered.

The 15° angle snapping the study mentions is a property of the *gizmo*, not the
field: the engine takes whatever line it is given. Snapping belongs in ClaySpace.

## What Changes

- **`cdeform_pose_line`**: anchor, end, rotation axis, angle. The weight ramps
  from 0 at the anchor to 1 at the end along the projection onto the segment,
  shaped by an easing curve, and the rotation is about the axis **through the
  anchor** so the anchor stays put.
- **The deformer record widens by one slot.** Ten parameters against the current
  nine; the record grows from 11 floats to 12 and the extension array from 5 to
  6. The tape is rebuilt on every compile so its width is free, and the
  serializer already takes its float count from the deformer type, so documents
  written before this are unaffected.
- **Bounds** cover the arc, not just its endpoints: the hull of the original and
  fully-rotated corners, dilated by the sagitta of the swept angle, so a
  half-turn cannot bulge outside the bound.
- **Exactness** from the item's extent about the axis against the ramp length —
  the same shape of reasoning twist and bend already use.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: pose gains a line-gradient region.
- `python-bindings` and `c-abi`: it joins the surface.

## Impact

- `include/clay/kernel/{deform,exactness,tape}.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build}.cpp`, both bindings, tests, parity corpus, docs.
- ABI 0.11.0 — additive.
- Leaves mask-defined regions to `add-mask-field`, as before. With this landed
  pose covers two of the three sources, and the third is named.
