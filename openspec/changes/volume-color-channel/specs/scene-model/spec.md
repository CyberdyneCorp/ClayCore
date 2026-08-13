# scene-model — consolidation stops discarding colour

Delta for `volume-color-channel`.

## ADDED Requirements

### Requirement: Consolidation preserves the colours it bakes
Consolidating a layer SHALL write the per-item colours into the resulting volume's colour channel. Consolidation is advertised as changing what a layer COSTS rather than what it looks like, and collapsing every colour in a layer to the one on the resulting node contradicts that: a consolidated character currently loses the distinction between skin and armour.

The volume's colour SHALL take precedence over the node's where a sample carries one, and the node's colour SHALL remain the answer outside the sampled box and for a volume with no colour.

`Op::Paint` SHALL continue to override both. It is the operator whose whole purpose is to set colour, and a volume that ignored it would make painting over a consolidated layer impossible.

Consolidated output is therefore NOT byte-identical to what this build produced before. The bit-identity gate that guards consolidation SHALL be re-baselined deliberately, in the same change, with the reason recorded — a silent re-baseline of a gate that exists to catch silent change would be the worst possible way to ship this.

#### Scenario: A two-colour layer consolidates to a two-colour volume
- **WHEN** a layer holding a red item and a blue item is consolidated and the result is evaluated at points inside each
- **THEN** the reported colours are red and blue, not one colour for both

#### Scenario: Painting over a consolidated volume still works
- **WHEN** a Paint operation is applied over a consolidated coloured volume
- **THEN** the painted colour is reported, overriding the volume's own
