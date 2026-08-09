# Proposal: sculpting verbs for an SDF layer

## Why

Every sculpting verb in the engine is a `VoxelGrid` method. An SDF layer has
none:

```
VoxelGrid: sculpt_smooth, sculpt_pinch, sculpt_inflate, sculpt_magnify,
           sculpt_flatten, sculpt_scrape, sculpt_smudge, sculpt_grab,
           sculpt_carve_alpha, sculpt_fill_cavities
SDF Layer: — none of them —
```

So the two representations are not two backends for one toolkit. The voxel side
has a sculpting toolkit; the SDF side has a *modelling* toolkit — add an item,
combine it, deform it — and an artist working there cannot smooth, pinch or
inflate anything.

The gap is not theoretical, and it is what makes an SDF figure look the way it
does. Building `examples/34_organic_character.py`, the seam where an arm meets
the torso needed to be *smoothed*, and there is no verb that does it. The only
lever available is the blend radius on the union — which is a property of the
join, chosen once when the item is added, and global to that join. Turn it up
and the smooth-min swallows the arm; turn it down and the arm reads as a
separate tube stuck on the body. There is no state in between, and no way to
say "soften this shoulder and leave the elbow alone".

`RELIEF` and `INCISE` are not substitutes. They offset the accumulated field
weighted by a region, which raises or cuts *along the existing surface normal*.
That builds a pec or carves a seam. It cannot average a surface with itself,
which is what smoothing is.

## What this is not

**Not a request to voxelise.** The point of the SDF representation is that it
stays a resolution-independent field, and a verb that rasterised the layer,
sculpted it and re-fitted would throw that away.

**Not a new deformer.** Deformers warp an item's *input space*, so they act on
one item and follow it. A sculpting verb acts on the accumulated field of the
layer at a place in the world, which is a different thing: it must apply to
whatever happens to be there, including the seam between two items where
neither one owns the surface.

## Approach

Add a family of layer-scoped verbs that append a *field-shaping instruction* to
the tape, in the way `RELIEF` already does, rather than editing geometry:

    layer.sculpt_smooth(centre, radius, strength, falloff)
    layer.sculpt_inflate(centre, radius, amount, falloff)
    layer.sculpt_pinch(centre, radius, amount, falloff)
    layer.sculpt_flatten(centre, radius, normal, offset, falloff)

Each records a bounded region and its parameters. The kernel applies them after
the item chain, weighted by the region so the effect falls off to nothing at
the boundary and the field stays continuous.

Smoothing a distance field is the one that needs design work and is the reason
this is a proposal rather than a task. Averaging `d` over a neighbourhood is
not free in a field with no lattice: it means sampling the accumulated field at
several offsets per evaluation, which multiplies the cost of every sample
inside the region and — because the result is no longer the exact distance to
anything — degrades the Lipschitz bound the raymarcher relies on. The design
has to state the sample pattern, the cost multiplier, and the Lipschitz factor
the verb declares, in the way every other inexact operation in `sdf-kernels`
already does.

Inflate, pinch and flatten are cheaper: each is a weighted offset or a weighted
pull toward a plane or an axis, all expressible without resampling neighbours.
They may well land first.

## Open questions

- **Ordering.** Do the verbs apply strictly after the whole item chain, or can
  they be interleaved so later items combine against a smoothed surface? Later
  items combining against a smoothed accumulator is what an artist expects and
  costs a more complicated tape.
- **How many samples smoothing takes**, and whether the count is fixed or
  derived from the radius.
- **Whether the verbs are undoable commands** (they should be, to match every
  other layer edit) and how they serialise into `.clayspace`.
- **Whether `grab` and `smudge` belong here at all.** Both translate material,
  which on a field is what a deformer already does; `move_surface` covers part
  of it. This change deliberately proposes the four that have no equivalent.

## Impact

`sdf-kernels` gains a class of field-shaping operation and must declare its
exactness and Lipschitz cost. `scene-model` gains the commands. `c-abi` and
`python-bindings` gain the entry points. Nothing existing changes behaviour;
this is additive.
