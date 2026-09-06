# scene-model

## ADDED Requirements

### Requirement: A computed placement is the placement command with a computed position

A placement derived from a layer's own content SHALL be expressed as the
existing layer placement command with a computed translation, and SHALL NOT
introduce a new command, a new node property or a new stored field. The command
already carries the transform and the per-axis scale together, so one such
placement is ONE command: one undo step whose inverse is the previous
placement, and one invalidation.

The position SHALL be the only component the computation writes. Because a
layer's world map applies its position outermost and its per-axis scale
innermost, adding a world-space delta to the position translates the layer's
world content by exactly that delta whatever rotation and whatever per-axis
scale the layer carries — so no case analysis over squashed and unsquashed
layers exists, and none SHALL be introduced.

The command SHALL be recorded even when the computed placement equals the
current one. The geometric no-op is not detectable exactly — recomputing the
bound from an already-placed layer does not return the same float — so
suppressing it would require a tolerance in world units, and an undo depth that
depended on the geometry would leave a host unable to predict what its own
button cost.

Nothing about the document's serialization SHALL change. A computed placement
saves as the placement it produced, which every existing format minor already
stores.

#### Scenario: One press is one undo step
- **GIVEN** a document with undo enabled and a layer holding material
- **WHEN** a computed placement is applied and undone once
- **THEN** the layer carries exactly the placement it had before, rotation and both scales included
- **AND** redoing restores the computed placement

#### Scenario: A press that moves nothing is still one step
- **WHEN** the same computed placement is applied twice
- **THEN** two commands are recorded, and two undos return the layer to where it started

#### Scenario: A squashed layer needs no separate path
- **GIVEN** a layer carrying a rotation and three different per-axis scale factors
- **WHEN** a computed placement translates it
- **THEN** its bounds move by the computed delta and keep their size

#### Scenario: The document still saves at the same minor
- **WHEN** a document whose layer took a computed placement is saved and reloaded
- **THEN** it loads at the format minor it already used, with the placement it was given

### Requirement: A computed placement reads the tight content bound

A placement computed from a layer's extent SHALL read the TIGHT world-space
bound of the layer's content — the box a camera frames — and SHALL NOT read the
influence bound. The influence bound is dilated by blend support and by the
chain pad, and a placement computed from it would leave the content hovering by
exactly that dilation.

The tight bound SHALL be the same one every representation already answers
with, so an SDF, a voxel and a mesh layer take one rule rather than three.

What the tight bound is NOT SHALL be stated where the placement is documented,
because each is a way the surface can miss the plane the artist named: a
SUBTRACT item contributes its own box even though it removes material, a smooth
blend can bulge past the boxes of both its operands, and hidden items are
excluded from the box entirely.

The exclusion of hidden items SHALL be the intended behaviour rather than a
limitation: the placement follows the silhouette the artist can see, and hiding
the lowest item therefore changes where the next placement lands.

#### Scenario: The influence bound is not what is read
- **GIVEN** a layer whose items blend with a wide support
- **WHEN** it is placed against a ground height
- **THEN** its tight bound's low face is at that height, rather than its influence bound's

#### Scenario: A hidden item does not hold the layer up
- **GIVEN** a layer whose lowest root is hidden
- **WHEN** it is placed against a ground height
- **THEN** the lowest VISIBLE content sits at that height

#### Scenario: All three representations take one rule
- **GIVEN** an SDF layer, a voxel layer and a mesh layer holding equivalent content at the same placement
- **WHEN** each is placed against the same ground height
- **THEN** each reports the same low face afterwards
