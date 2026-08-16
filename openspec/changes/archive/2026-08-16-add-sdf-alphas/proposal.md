# Proposal: alphas on SDF layers

## Why

Detail work in every competing sculptor is alpha-driven. Pores, fabric weave,
scales, stitching, panel-line stamps, skin grain — a ZBrush artist reaches for
an alpha within the first hour, and the answer here today is that
`VoxelGrid::sculpt_carve_alpha` exists and nothing equivalent does on an SDF
layer.

That is the wrong asymmetry to leave standing. The SDF side is the
non-destructive one: it is where an edit stays re-editable forever, and it is
the representation the engine's structural advantages live in. Restricting the
one detailing technique artists actually use to the *baked* representation
inverts the value proposition — you can have detail or you can have
re-editability, and choosing is exactly what this engine exists not to make
people do.

`docs/sculpt_comparison.md` files this under Tier 1, and the parity table's
Alphas row reads "voxel side only so far".

## Approach

**An alpha is a deformer, not a primitive**, and that choice is the proposal.

A primitive shaped like the stamp would ADD material in the stamp's shape. That
is not what an alpha does. An alpha *modulates an existing surface* — it makes
pores in skin that is already there — which is a distance OFFSET under finite
support, and the engine already has that shape twice over: `cdeform_noise`
offsets the whole item, and `cdeform_blob` offsets it under the same radial
falloff `grab` and `magnify` use. An alpha is `blob` with the fractal noise
replaced by a lookup into a caller-supplied 2D stamp.

That framing also makes the two representations mean the same thing, which is
the standard `sculpt_grab` and the `grab` deformer are already held to: the
voxel verb modulates cells the model already has, and so does this.

**The engine decodes no images.** A host with an alpha has already loaded a
PNG; it hands over `width * height` samples in [0, 1]. That rule is already
stated for the voxel verb and is not relaxed here — it is what keeps an image
decoder out of a library that runs on five backends.

The samples do not fit a fixed record, so they ride in the tape's blob exactly
as a bend curve's guide and a lattice's offsets do. That path is established;
this is its third user, not its first.

## The part that is not mechanical

**The Lipschitz bound.** Every other deformer here declares how far it can
stretch space, and the raymarcher's step size is derived from that rather than
tuned. An alpha's offset is `amplitude * falloff(p) * stamp(u, v)`, so its
bound needs the stamp's own steepness — and a stamp is caller data that the
engine sees for the first time at build time.

The honest bound comes from the largest *difference between adjacent samples*
divided by the texel size, not from the largest sample value. `cfi_lattice`
already learned this distinction: a bound taken from magnitudes rather than
differences is both wrong and uselessly loose, because a stamp of all-ones is
perfectly flat and displaces nothing while having the largest possible values.

A high-frequency stamp therefore costs step scale honestly, and a smooth one
costs almost nothing. That is the right trade to expose rather than hide: an
artist stamping 4K skin grain onto a small region should see the cost, not
discover it as a rendering artifact.

## Open questions

- **Orientation.** A stamp needs a plane and an up direction to be placed. The
  voxel verb takes only a `direction` and lets the projection pick the rest,
  which is fine when the result is quantised to cells and not fine when it is a
  field a host will re-edit. Likely: an explicit tangent, with a documented
  default derived from the direction.
- **Tiling versus one-shot.** Fabric and scales want to repeat; a stitch or a
  logo does not. Clamping outside the footprint is the conservative default and
  matches every other finite-support deformer here; tiling can be a flag if it
  earns one.
- **Sampling.** Bilinear is the obvious choice, and nearest would alias badly
  against a raymarcher. Whether the bound assumes bilinear (it must) needs
  stating where the bound is derived rather than left implicit.

## Impact

`sdf-kernels` gains the deformer and its exactness rule. `brush-engine` gains
the placement helper, so a host stamping along a stroke does not recompute the
frame itself. `c-abi` and `python-bindings` gain the surface; the C ABI entry
point is its own call rather than a `clay_item_add_deformer` case, because a
variable-length payload does not fit that signature — the same reason
`clay_item_add_bend_curve` exists.

Additive throughout: an item with no alpha behaves exactly as today, and a
stamp that is entirely zero is exactly the identity.
