# scene-model

## ADDED Requirements

### Requirement: A layer carries a per-axis scale

A layer SHALL carry three scale factors beside its transform, composed
innermost in the layer's own frame — before its rotation and translation — so
that an item's world map is
`layer.xform · diag(layer.scale_axes) · node.xform · diag(node.scale_axes)`.
The layer transform's uniform factor SHALL remain the similarity scale and the
three axes SHALL modulate it, so a triple of ones is the identity.

A layer whose three factors are equal SHALL behave exactly as one carrying the
uniform factor alone, and SHALL compile to bit-identical tape.

A non-uniform layer scale SHALL be reported through the field's exactness, not
through its Lipschitz bound: the evaluated distance SHALL be multiplied by the
product of the smallest component of each per-axis scale in the composition,
which never overestimates the true distance, and the safe step scale SHALL NOT
move.

Bounds, influence bounds and picking SHALL honour the three factors. A world
radius mapped into a squashed frame SHALL be divided by the LARGEST component,
so a gesture never reaches outside the region it named.

#### Scenario: Three equal factors are the uniform layer
- **WHEN** a layer's per-axis scale is set to three equal factors
- **THEN** the compiled tape is byte-identical to the same layer carrying that factor as its uniform scale

#### Scenario: A squashed layer squashes its items
- **WHEN** a layer holding a unit sphere is scaled 3x on one axis
- **THEN** the field's zero set is an ellipsoid three units along that axis and one along the others, and the layer's reported bounds contain it

#### Scenario: The field stays marchable
- **WHEN** a layer carries a non-uniform scale
- **THEN** the tape reports itself inexact and its safe step scale is unchanged from the uniform case

#### Scenario: A drag on a squashed layer reaches its surface
- **WHEN** a surface drag is applied at a point on a layer-squashed item's surface
- **THEN** the surface moves there, and the falloff never reaches outside the radius the gesture named

### Requirement: A lattice cage refuses a frame it cannot describe

The transformed-lattice deformer carries its item-to-cage placement as a rigid
transform with a uniform scale. The map a cage actually needs is
`cage.placement⁻¹ · layer.xform · diag(layer.scale_axes) · node.xform ·
diag(node.scale_axes)`, which is a general affine map whenever either per-axis
scale is non-uniform — not a similarity, and not a similarity composed with one
diagonal either.

Until that record is widened, a lattice gizmo over a per-axis-scaled layer SHALL
be REFUSED rather than placed through a record that cannot hold its map. A cage
placed through one would warp the item in a space it does not occupy, silently
and with no error, which is worse than either the refusal or the absent feature.

The refusal SHALL name the layer's scale as the reason, so a host can offer the
uniform gizmo instead of reporting a failure it cannot explain.

A cage over a layer carrying no per-axis scale SHALL behave exactly as before.

#### Scenario: A gizmo over a squashed layer is refused
- **WHEN** a lattice gizmo is applied to a layer carrying a non-uniform scale
- **THEN** it produces no warps and reports that the layer's per-axis scale is why

#### Scenario: An unsquashed cage is unchanged
- **WHEN** a lattice gizmo is applied to a layer carrying no per-axis scale
- **THEN** the warps are the ones it produced before
