# Proposal: flattening an SDF surface

## Why

Voxel layers have `sculpt_flatten`. SDF layers have nothing — the same
asymmetry `add-sdf-relax` just closed for smoothing, and the same fix is
available now that sampled volumes exist.

Flatten is what hard-surface work reaches for constantly: a planar facet blended
into surrounding form. Without it, the only way to get a flat face on an SDF
layer is the cut tool, which removes everything past a plane across the whole
sweep. That is a different operation — a trim is global to its prism and has no
falloff, so it cannot put a facet on one part of a shape and leave the rest.

## This is not the Clip brush, and that is deliberate

This row was first raised as "add the Clip brush". It should not be.

ZBrush's Clip moves vertices onto the plane instead of deleting them. Ask what
solid that bounds: the clamp map `c(p) = p − max(0, p·n − d)·n` sends a solid
`S` to `(S ∩ H⁻) ∪ proj(S ∩ H⁺)`, and the second term lies *on* the plane —
measure zero, no volume. **As a solid, Clip is exactly Trim**, which the cut
tool already does. Clip's distinctive appearance in ZBrush is the flattened
polygons surviving as a zero-thickness fin coplanar with the cut face, which is
a mesh artifact and the reason Clip is so often followed by Dynamesh.

A field cannot represent a zero-thickness fin, and manufacturing one to imitate
a result users then delete would be building a bug on purpose.

Worth recording because it is a trap: a clamping deformer does **not** clip.
`g(p) = (p.x, p.y, min(p.z, 0))` evaluates the field at the plane for every
point beyond it, which *extrudes* the cross-section to infinity — the opposite
of cutting it off.

## What flatten means here

The same thing it means for voxels, because two representations that share a
verb's name must share its meaning. `sculpt_flatten` pulls the surface onto a
plane through the brush centre: material on the + side goes, hollows on the −
side that touch material are filled. So flatten is **two-sided** — it is not a
subtract.

## The design question: how to move the field

Two candidates.

**Displace the samples.** Move each stored sample toward the plane. Rejected:
samples live on a fixed lattice, so "moving" one means resampling, and the
lattice is exactly what makes the volume cheap to evaluate.

**Blend the value.** The plane is itself a signed distance function,
`p ↦ n·(p − o)`. Flattening is then pulling the field's value toward the
plane's value where the brush acts. Full weight leaves the field equal to the
half-space, so the surface *is* the plane. This composes with the existing
machinery: it is a rewrite of stored samples, exactly like relax.

Taking the second.

## The part that needs care: a varying weight is not free

Relax could lean on averaging being non-expansive. Blending toward a plane
cannot, and it is worth being explicit about why.

For a weight `w(p)` that varies across the region,

    ∇[(1−w)·d + w·q] = (1−w)∇d + w∇q + (q − d)·∇w

The first two terms are a convex combination of two 1-Lipschitz fields and are
fine. **The third is the problem**: `|q − d|` is how far the field has to move,
and `|∇w|` goes as the reciprocal of the falloff width, so a strong correction
tapered over a short distance makes the field arbitrarily steep — and a field
steeper than it declares is one a raymarcher steps through.

The answer is to **bound how far a single pass may move the value** and iterate,
which caps the third term at `step × |∇w|` regardless of how far the surface
ultimately travels. That also mirrors relax's `iterations`, so the two verbs
have the same shape for the same reason rather than by coincidence.

The step limit and the falloff are therefore related, not independent, and the
implementation will widen a falloff that is too narrow for the step it was
given — the same treatment relax gives a taper too narrow to hide its kernel.

## What Changes

- **`field::flatten`**: pull a sampled volume's field toward a plane inside a
  region, returning a new volume. A plane (point + normal), a strength, a
  bounded step, iterations, and the same region/falloff as relax.
- **Python bindings**, and a C ABI entry point beside `clay_item_volume_relax`
  so an imported scan can be faceted from an app.
- **An example**, and tests that measure the declared Lipschitz rather than
  assuming it.

## What this change does not do

- **No live/parametric flatten.** Flatten bakes, for the reasons
  `add-sdf-relax` records: a general flatten must act on a bump in the middle of
  one item, which no reweighting of an edit list expresses.
- **No curved flatten surface.** A plane, not ZBrush's ClipCurve. A curved
  target is the same blend against a different field and is additive later.
- **No automatic plane from the surface.** The caller supplies point and
  normal, as with the cut tool: no camera and no picking enters the engine.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `python-bindings`.

## Impact

- New `include/clay/field/flatten.h` and its source; the Python bindings, the
  C ABI, tests, docs, an example.
