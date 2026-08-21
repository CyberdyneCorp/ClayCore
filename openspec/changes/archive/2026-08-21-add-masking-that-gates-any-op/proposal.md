# Proposal: masking that gates any operation

## Why

`docs/sculpt_comparison.md` calls this "the single biggest missing concept",
and it is the last of #118's Tier 1 workflow gaps.

Masks exist and work. What they gate is **authoring**: a voxel edit consumes
the mask per cell as it writes, and an SDF edit consumes it when the stroke
engine turns a stroke into items. `include/clay/voxel/mask.h` states the rule
plainly — a mask protects a region from *what you do next*, and does not gate
items already in the edit list.

That covers brushes and nothing else. In ZBrush, masking protects the
**surface** from *any* operation, and the operation an artist most wants
protection from is not a brush at all: it is the next **boolean**. Cut a hole
through a head and the mask over the ear does nothing, because a subtract is an
item in the edit list rather than a stroke passing through the mask.

The asymmetry also shows up as a workflow cliff. An artist masks a region,
reaches for a brush, and it is respected; reaches for a boolean, a deformer, or
a whole-layer operation, and it silently is not. Nothing tells them which is
which.

## The design question

The epic states it: **a field-level clamp on the accumulated result, or
per-item participation** — and either way it must not break per-brick culling.

### Why the field-level clamp does not work here

"Protect the surface" reads as: keep the old field where the mask is 1. Written
out, that is `mix(d_before, d_after, 1 - mask)`.

There is no `d_before`. A claycore document is not a state that operations
mutate — it is an edit list that evaluates from scratch, which is the property
the whole engine is built on. `d_before` would have to be a *snapshot*, and a
snapshot is exactly the baked, un-re-editable thing masking is supposed to
protect you from needing. It would also have to be shipped to every backend,
which is a sampled volume of the whole layer per masked operation.

### Per-item participation is already the tape's shape

The accumulator IS `d_before` at the moment an item combines. The tape walks
the edit list accumulating a field; when item *k* combines, the accumulator
holds precisely "everything before item k". So

    d = mix(combine(acc, item, op), acc, mask(p))

needs no snapshot at all. At `mask = 1` it is exactly `acc` — the item does not
participate — and at `mask = 0` it is exactly the unmasked result. Both ends are
exact rather than approximate, which is the property every gate in this engine
is held to.

This is not a new mechanism either. `ccombine_transition_linear` and
`ccombine_transition_radial` already mix the accumulated field against an item's
by a spatially varying weight; a masked item is the same operation with the
weight read from a mask instead of from an analytic ramp.

## Approach

**Gate participation per item, with the mask carried as a sampled volume.**

The hard part is not the mix — it is that the mask has to be readable *during
evaluation*, on five backends, where today it is a host-side lattice consumed
before evaluation begins. Rather than teach every backend a new sparse uint8
format, use the payload the engine **already** ships and evaluates in-tape:
`FieldVolume`, the sparse brick volume behind `ctape_volume`. It is already
blob-carried, already uploaded, already has an exactness story, and already
survives the round trip.

**But a `FieldVolume` is a narrow-band SIGNED DISTANCE volume, not a scalar
one**, and that is not a detail to wave past. Storing a painted [0,1] mask in it
directly would mean treating the paint values as distances: the band would keep
samples near 0.5 and discard the rest, and the Lipschitz bound would be set by
however hard the artist's brush edge happened to be.

So the gate wants the **signed distance to the protected region's boundary** —
threshold the painted mask, measure the distance to that region — and applies
its own falloff across an explicit width.

**That conversion already exists.** `brush::mask_to_field` measures exactly
this — "signed distance to the boundary of { mask >= threshold }, negative
inside, 1-Lipschitz, tape-expressible, blob-carried" — and it reaches the C ABI
as `clay_mask_to_field`. It was built for `mask_extrude` and its header records
the same reasoning this proposal re-derived. So the foundational half of this
change is done, and what is left is the gate itself, which makes the change
meaningfully smaller than it first appears.

The gate reads:

    weight(p) = smoothstep(-w, +w, distance_to_protected(p))

Three things follow, and the third is the reason this is the right shape rather
than a workaround:

- the band is exactly where the interesting values are, and bricks wholly
  inside or wholly outside the protected region cost nothing, which is what
  `FieldVolume` is already built to exploit;
- a distance field is 1-Lipschitz, so the mix's cost is `1/(2w)` times the
  smoothstep's slope — a number derived from a width the ARTIST chose, not from
  the resolution of the lattice they painted on;
- it decouples the bound from how the mask was painted. A hard-edged paint
  stroke and a soft one give the same bound and the same falloff.

**What it costs: painted softness is re-derived rather than preserved.** A mask
painted with a deliberately soft edge becomes a hard region plus an explicit
falloff width. That is a real loss and worth stating; the trade is a bound that
can be stated in advance and a falloff an artist can change after the fact
without repainting, which on balance is the better tool.

## What it costs, stated up front

**Lipschitz.** Mixing two fields by a spatially varying weight is not a
distance, and `cfi_transition` already charges the honest price:
`|d_a - d_b| · Lipschitz(weight)`. Because the gate's weight is a smoothstep of
a true distance over a chosen width `w`, that Lipschitz is `slope/(2w)` — so a
NARROW gate edge costs step scale and a soft one costs little. That is the trade
to expose rather than hide, and it is under the artist's control rather than a
consequence of the lattice they painted on. A gate that is uniform over an
item's influence must cost nothing at all, and that is testable.

**Culling stays valid, and this is the part worth checking rather than
assuming.** Masking can only *remove* an item's effect, never extend it beyond
where it already acted, so an item's existing influence bound remains a
superset. Per-brick culling therefore needs no change at all. A mask that
*grew* a bound would be the dangerous case, and this formulation cannot produce
one.

**Memory.** A baked mask volume is per masked operation, not per document, and
it is the same sparse representation the engine already budgets for.

## Open questions

- **Where the mask attaches.** To the item (this gate, this boolean), to the
  node, or to the layer. Per item is the smallest thing that answers the
  motivating case and composes; a layer-wide mask is expressible as the same
  gate repeated, but that may be the wrong ergonomics.
- **Baking policy.** Painting a mask and baking it to a volume on every edit
  would be wasteful; the bake wants to be lazy and cached against the mask's
  revision, the way brick content already is.
- **The threshold.** 0.5 is the obvious cut for "protected", but a mask painted
  with partial strength everywhere has no clean boundary, and the honest answer
  may be to expose the threshold rather than assume it.
- **The voxel side.** Voxel edits already consume masks per cell at apply time
  and need nothing here. The two must keep *meaning* the same thing, which is
  the standard `sculpt_grab` and the `grab` deformer are held to — and that is a
  test, not a note.
- **Whether an unmasked document pays anything.** It must not: no volume, no
  extra combine mode reached, byte-identical tape. That is a gate on the change,
  not an aspiration.

## Impact

`sdf-kernels` gains the gated combine and its exactness rule. `scene-model`
gains the attachment point and its serialization. `voxel-engine` gains the bake
from a painted `MaskField` to a volume. `c-abi` and `python-bindings` gain the
surface. Additive: a document with no masked item evaluates and serializes
exactly as today.
