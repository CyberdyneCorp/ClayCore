# Proposal: local surface displacement — grab and pose

## Why

Every deformer the engine has acts on a *whole item*: twist the box, bend the
capsule, elongate the sphere. None of them lets a sculptor push on a patch of
surface and have only that patch move. In the 3DCoat corpus, Move/Grab and Pose
are the **first and fourth most-used tools** — and in ZBrush terms the entire
Manipulation category (Move, Move Elastic, Snake Hook, Nudge) has no analog
here. It is the largest remaining gap between claycore and a sculpting app, and
unlike the brush inventory it is not something more primitives can paper over.

The gap is architectural rather than missing arithmetic. On a mesh, grab is
trivial: displace the vertices under the cursor. Here there are no vertices —
an SDF is a function and voxel occupancy is binary. So "move this bit of
surface" has to become "warp this region of space", which is a deformer with
*finite support* rather than a new brush.

That framing is what makes it tractable, and it is already most of the way
built: `bend_linear` displaces by a vector ramped along a segment, and
`bend_radial` ramps across a radial band. Grab is the same shape of thing —
displacement ramped radially about a point — and it lands in the deformer chain
with the machinery those two already proved.

## Scope: both representations

ClaySpace sculpts in SDF and voxels, and wants hard-surface *and* organic work,
so this covers both:

- **SDF**: a `grab` deformer opcode. Finite support, Lipschitz-tracked,
  composes in the chain like any other.
- **Voxels**: a `voxel_grab` verb resampling occupancy through the same inverse
  map, so the two representations mean the same thing by the same maths.

Pose — a *transform* weighted across a region rather than a translation — is
specified here as a second requirement because it shares the entire
region-weight mechanism. Region weights from a painted mask are deliberately
left out: they depend on `add-mask-field`, which is not built.

## What Changes

- **`cdeform_grab`**: centre, radius, displacement vector, easing curve. Outside
  the radius the map is the identity, so the item's influence bound dilates by
  the displacement and no more — brick culling stays tight, which a global warp
  would destroy.
- **`cdeform_pose`**: a rigid transform (rotation about a pivot plus
  translation) applied with the same radial weight, for the taper-and-repose
  motion Pose is used for.
- **Front-facing only**, optional on both. 3DCoat's most-cited Move complaint is
  that it deforms the *back* of the form as well; the fix is to gate the weight
  on the local gradient facing the stroke direction, and it is cheap because the
  gradient is already available.
- **`voxel_grab`**: same centre/radius/displacement/falloff, resampling
  occupancy and palette index from the inverse-displaced position. Binary
  occupancy means this aliases where the displacement exceeds a cell — the spec
  states that rather than pretending otherwise.
- **Both bindings**, and a parity scene so every backend agrees.

## Why the parameters fit

Grab needs seven floats (centre, radius, displacement) against four base slots.
The deformer record already carries five extension slots, added for
`bend_linear`, and the serializer takes its float count from the deformer type —
so this needs no format change, exactly as that one did.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: region-weighted displacement joins the deformers.
- `voxel-engine`: grab joins the sculpting verbs.
- `python-bindings` and `c-abi`: both reach the new surface.

## Impact

- `include/clay/kernel/deform.h` (new warps), `tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build}.cpp`, `src/voxel/sculpt.cpp`, both bindings, tests, parity corpus, docs.
- ABI 0.10.0 — additive.
- Depends on nothing. `add-mask-field` would later extend the region sources
  from line/sphere to painted masks; that is a separate change and this one is
  designed not to block on it.
- Non-goals: mask-defined regions, snake-hook style repeated dragging (a stroke
  concern for `add-brush-stroke-engine`), and mesh-style vertex manipulation,
  which no representation here has.
