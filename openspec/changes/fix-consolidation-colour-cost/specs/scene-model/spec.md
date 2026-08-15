# scene-model — a grey layer should not pay for colour

Delta for `fix-consolidation-colour-cost`.

## MODIFIED Requirements

### Requirement: Consolidation preserves the colours it bakes
Consolidating a layer SHALL write the per-item colours into the resulting volume's colour channel. Consolidation is advertised as changing what a layer COSTS rather than what it looks like, and collapsing every colour in a layer to the one on the resulting node contradicts that: a consolidated character currently loses the distinction between skin and armour.

The volume's colour SHALL take precedence over the node's where a sample carries one, and the node's colour SHALL remain the answer outside the sampled box and for a volume with no colour.

`Op::Paint` SHALL continue to override both. It is the operator whose whole purpose is to set colour, and a volume that ignored it would make painting over a consolidated layer impossible.

Consolidation SHALL NOT fill a colour channel when the absorbed set cannot produce more than one colour. Filling it is a second evaluation of the tape at every surviving sample, and where every absorbed node carries the same colour the result is that one colour repeated — which the node's own colour already reports, by the rule above. The absorbed set can produce more than one colour when two or more distinct node colours appear in it, or when any absorbed node is a volume whose samples carry colour of their own; a volume with a colour channel has one node colour and many sample colours, so a test on node colours alone SHALL NOT be the whole condition.

The decision SHALL be made from the absorbed set rather than from the samples, so that a layer which does not need the pass never pays it.

#### Scenario: A two-colour layer consolidates to a two-colour volume
- **GIVEN** a layer holding a red item and a blue item
- **WHEN** the layer is consolidated and the result is sampled inside each item
- **THEN** the reported colours are red and blue, not one colour for both

#### Scenario: A one-colour layer consolidates without a colour channel
- **GIVEN** a layer whose items all carry the same colour
- **WHEN** the layer is consolidated
- **THEN** the resulting volume has no colour channel, the resulting node carries that colour, and sampling anywhere reports it

#### Scenario: A coloured volume is re-consolidated
- **GIVEN** a layer holding one item, itself a volume whose samples carry two colours
- **WHEN** the layer is consolidated again
- **THEN** the colour pass is taken and both colours survive, even though the absorbed set holds a single node colour

#### Scenario: Paint overrides a consolidated volume's colour
- **WHEN** a Paint operation is applied over a consolidated coloured volume
- **THEN** the painted colour is reported, overriding the volume's own
