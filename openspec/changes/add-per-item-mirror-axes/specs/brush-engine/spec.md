## ADDED Requirements

### Requirement: A drag reaches an item through the item's own mirror copies
An item that carries its own mirror axes SHALL be tested against the images its OWN copies make of a drag: the drag, one reflection per axis the item sets, then the layer's radial rotations if the item participates in them. It SHALL NOT be tested against the layer's images. An item that inherits SHALL see the layer's images exactly as before, bit for bit.

This is the intended behaviour. A Move or magnify drag SHALL move both sides of an item that kept its own X twin, even with the layer's mirror off, because both sides are that item. It SHALL move only the touched side of an item whose own axes are none, whatever the layer's mirror is. A host that wants a formerly mirrored item to move on one side only sets that item's own axes to none.

The reach a gesture reports for invalidation SHALL include the reflections for the dragged items' own axes, so a twin that the layer's mirror does not name is not served stale.

#### Scenario: An own-axes twin is reachable with the layer mirror off
- **WHEN** an item carries its own axes X on a layer with no mirror, and a drag is prepared at its twin
- **THEN** the item is reached through two images, and the reported drag images include the reflected ball

#### Scenario: An item held at none moves on one side
- **WHEN** an item carries its own axes of none on a layer mirroring X, and a drag is prepared at the layer's reflection of it
- **THEN** the item is not reached, and a drag on the item itself reaches it through one image
