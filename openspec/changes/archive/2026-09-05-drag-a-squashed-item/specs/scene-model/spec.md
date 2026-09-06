# scene-model — drag a squashed item

Delta for `drag-a-squashed-item`.

## ADDED Requirements

### Requirement: A warp is authored in the space the deformer chain runs in
An item's deformer chain runs on the point AFTER the whole inverse has been applied — the per-axis scale included, because it is innermost. Any tool that authors a deformer from world-space input SHALL therefore map its input through that same whole inverse, and not through the placed frame alone.

Composing only the layer and node transforms SHALL NOT be treated as the item's frame. It was the whole story before an item could carry a per-axis scale and is not one now: a world point on the surface of an item stretched by a factor maps, under the placed frame alone, to a local point that far outside the primitive, and a falloff authored there reaches nothing.

A drag on the surface of a stretched item SHALL produce the same local warp as the same drag on the corresponding point of the unstretched one, since the local geometry is identical and only the frame differs.

#### Scenario: A drag reaches a stretched item's surface
- **WHEN** an item scaled by a factor on one axis is dragged at the world point where its surface now sits
- **THEN** the surface moves, where before the tool produced a warp that reached no part of the item

#### Scenario: Stretched and unstretched drag identically
- **WHEN** the same drag is applied to a stretched item and to the unstretched one, each at its own corresponding surface point
- **THEN** both produce the same local grab centre and the same resulting field value at the dragged point

### Requirement: A scalar falloff under a non-uniform frame never over-reaches
A grab carries ONE radius, and a non-uniform frame maps the caller's world-space sphere to a local ellipsoid, so no scalar radius is exact. The radius SHALL be divided by the LARGEST scale factor, so that every world-space reach is at most the radius the caller named.

The direction SHALL be documented at the call as a choice, not left as arithmetic: the opposite division is equally arithmetic and takes geometry the caller did not enclose. Under-reach is recoverable by dragging again; over-reach is not, and it is the same conservatism the non-uniform distance operator applies when it multiplies by the smallest factor.

#### Scenario: A drag stays inside what was circled
- **WHEN** an item stretched by a factor of four is dragged with a given radius
- **THEN** the widest world-space reach of the resulting falloff does not exceed that radius, and geometry outside it is unchanged
