## MODIFIED Requirements

### Requirement: The field report names what degraded a layer

`clay_layer_field_report` SHALL report a layer's Lipschitz bound, its safe step
scale, the steepest volume and deformer chain it carries, its item and drawable
counts, whether consolidation is advised, and which kind of degradation it
suffers.

`clay_degradation` SHALL distinguish a layer degraded by stacked volumes or a
long edit list from one degraded by a chain of brushes, because the two have
different cures.

**`CLAY_DEGRADATION_DEFORMERS` SHALL state the regime in which its advice
holds.** Its documentation SHALL NOT say that consolidation is never the cure
for a brush chain: that is true above a measured step-scale floor and false
below it, where a redistanced volume marches in constant time while the chain
does not march at all. The text SHALL carry the floor, the measurement behind
it, and the fact that past some depth sphere-tracing stops finding the surface
rather than merely taking longer.

A host SHALL be able to distinguish "degraded, and baking would help" from
"degraded, and baking would make it worse" from the report alone, without
computing a threshold of its own.

`advises_consolidation` and `degradation` SHALL agree: a layer that advises
consolidation SHALL name a degradation, and a layer that names none SHALL NOT
advise.

#### Scenario: A brush chain is named even where the bake is refused
- **WHEN** a host reports a layer of one drawable carrying a shallow brush chain, below its own tolerance
- **THEN** `degradation` is `CLAY_DEGRADATION_DEFORMERS` and `advises_consolidation` is 0

#### Scenario: The same layer past the floor advises
- **WHEN** the chain has degraded the safe step scale below the scene model's floor
- **THEN** `degradation` is still `CLAY_DEGRADATION_DEFORMERS` and `advises_consolidation` is 1

#### Scenario: The two fields never disagree
- **WHEN** any layer is reported
- **THEN** `advises_consolidation` is 1 only where `degradation` is not `CLAY_DEGRADATION_NONE`

#### Scenario: A zero tolerance measures without advising
- **WHEN** a host passes `advise_below_step_scale` of 0
- **THEN** the report's measurements are filled, no degradation is named, and nothing is advised

#### Scenario: The advice call follows the report
- **WHEN** a layer that the report advises is passed to `clay_layer_consolidation_advice`
- **THEN** it receives parameters and a cost rather than a zeroed descriptor

#### Scenario: A non-SDF, protected or empty layer is not an error
- **WHEN** a host reports a layer that is not an editable field layer
- **THEN** the call succeeds, names no degradation and advises nothing
