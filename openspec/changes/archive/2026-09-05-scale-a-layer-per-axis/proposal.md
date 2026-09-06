# Proposal: a layer's transform takes one scale factor; a subtool gizmo needs three

## Why

ClaySpaceDesktop puts one ZBrush-style manipulator on a placed object and on a
whole layer. ZBrush's gizmo scales PER AXIS — the three boxes on the arms — and
users expect it. The host hides those boxes in scale mode, because an axis
handle would measure a stretch the engine cannot apply (#373).

Half of this shipped. `scale-an-item-per-axis` (#320, ABI 0.54.0) gave every
NODE placement a `float scale[3]`, and its task 2.1 deferred the layer
deliberately, naming why:

> `layer.xform * node.xform` is consumed as a rigid FRAME by `brush::move` and
> `brush::lattice_gizmo`, which place a manipulator and a cage in it, and a
> frame with a per-axis scale is not a `math::Transform` at all.

`drag-a-squashed-item` (0.54.1) then fixed `brush::move` for the NODE scale and
left the gizmo, naming the same blocker and where it belongs:

> `brush::lattice_gizmo` has the identical bug — its comment even names the
> assumption that broke. It is not fixed here because it cannot be without a
> FORMAT change [...] That belongs with the layer per-axis scale, which forces
> the same widening for its own reasons and takes a format minor anyway.

This is that change. It carries the deferred gizmo fix with it, which also
closes the standing gap where a cage is wrong inside a node that already carries
a per-axis scale.

## The composition, stated once

A layer's per-axis scale sits in the slot its uniform one already occupies —
innermost in the layer's own frame, before rotation and translation — exactly as
a node's does:

```
world_from_local = layer.xform · diag(layer.scale_axes)
                 · node.xform  · diag(node.scale_axes)
```

`xform.scale` stays the uniform similarity factor and the axes modulate it, so a
triple of (1, 1, 1) is the identity and every existing document composes
bit-identically.

## What a non-uniform scale costs the field, and what it does not

The same treatment the item arm already established, applied twice.

- The inverse goes into the tape's matrix and the DISTANCE is multiplied back by
  the **product of the two smallest components**. That is conservative rather
  than exact: for `A = L·D_l·R·D_n` with rotations `L` and `R`, the smallest
  singular value of `A` is at least `σ_min(D_l) · σ_min(D_n)`, so the value
  never overestimates the true distance.
- The field stays **1-Lipschitz** — dividing by `s` and multiplying back by
  `min(s)` can only shorten — so `safe_step_scale` does not move and no marcher
  slows down.
- What goes is `is_exact`, which is what `cfi_scale_nonuniform` says and all it
  says.
- A world-space RADIUS is divided by the **largest** component, the dual of the
  above: every world reach is then at most the region the artist circled.
  Under-reach is recoverable by dragging again; over-reach is not.

## The cage, which is the format change

`brush::lattice_gizmo` builds `local_to_cage = cage.placement⁻¹ · world` and
stores it in `Deformer::cage_xform`, a `math::Transform`. The map it actually
needs is

```
cage.placement⁻¹ · layer.xform · diag(L) · node.xform · diag(N)
```

which is a general affine map, not a `Transform` and not `Transform ∘ diag`
either — the earlier changes reasoned about the node scale alone and read it as
the latter. So `cage_xform` becomes an affine MATRIX for
`cdeform_lattice_xform`, which is the honest shape and closes both gaps at once:
the layer scale, and the node scale the cage was already dropping.

This is what takes the format minor. `kSceneMinor` and `kClaySpaceMinor` move to
16; the layer's `scale_axes` and the cage's matrix are gated on it and an older
stream loads with the defaults, which is the identity.

## What this does NOT change

- No new opcode and no wider tape record. The per-axis scale rides the inverse
  matrix and the scale slot the interpreter already reads, as the item arm's did.
- `clay_document_set_layer_transform` keeps its signature and its meaning. The
  per-axis form is a parallel entry point, matching how the node arm shipped.
- The single-factor READER refuses a squashed layer rather than reporting one of
  the three, exactly as `clay_layer_node_transform` refuses a squashed node.
