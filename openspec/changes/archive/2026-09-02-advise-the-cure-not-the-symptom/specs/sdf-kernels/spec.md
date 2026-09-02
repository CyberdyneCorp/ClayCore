## MODIFIED Requirements

### Requirement: A degrading chain is detectable
The engine SHALL make the degradation of a chained region-verb edit observable, from the declared Lipschitz and the safe step scale it already tracks, so that a host can act before the field becomes unusable rather than discovering it by eye.

TWO causes SHALL be distinguishable, because they are different faults with
different cures and an aggregate cannot tell them apart:

- a verb that samples a document and hands back a volume, so a second
  application samples a VOLUME — and outside its band a volume reports a lower
  bound rather than a distance, so the blend works from the wrong value;
- a verb that appends a domain warp per gesture, so a stroke multiplies the
  chain's Lipschitz factor without any volume being involved at all.

Each cause SHALL be reported as a FACTOR, not only as a count. A chain's length says nothing about what it costs the marcher, so a count cannot be weighed against the volume mechanism's steepness.

The report SHALL name which mechanism is responsible, so a host picks a cure rather than deriving one from a single number.

Any advice the report offers SHALL be keyed on that mechanism. Consolidation wins back exactly two things — the cost of walking an EDIT LIST, and the Lipschitz of STACKED VOLUMES, which the bake redistances away — so it SHALL NOT be advised for a layer that has neither, where it swaps a cheap analytic item for a dense volume and is measurably a loss.

The count of nodes and the count of nodes that are EVALUATED SHALL both be available. A group is a transform and a name; it contributes no field, so it is not an edit list to win back.

#### Scenario: A chain reports its own degradation
- **WHEN** a region verb is applied to the result of a previous region verb
- **THEN** the declared Lipschitz and the safe step scale reflect the chained cost, and both are readable before the next edit

#### Scenario: The report names which mechanism degraded the chain
- **WHEN** a host asks a layer what its chain costs
- **THEN** it is told the steepest sample Lipschitz among the layer's volumes and the steepest deformer chain factor among its items, alongside the aggregate and the name of the mechanism responsible

#### Scenario: The chain's factor separates chains of equal length
- **WHEN** two layers each carry one deformer, one gentle and one deep
- **THEN** they report the same chain LENGTH and different chain factors

#### Scenario: Consolidation is not advised where it does not apply
- **WHEN** a layer is one evaluated item carrying a brush chain, degraded past the caller's threshold
- **THEN** the mechanism is reported as the deformer chain and consolidation is NOT advised

#### Scenario: Consolidation is advised where there is something to absorb
- **WHEN** a layer degraded past the caller's threshold holds several evaluated items, or a volume whose samples are steep
- **THEN** consolidation is advised

#### Scenario: A group is not an edit list
- **WHEN** a single item degraded by its own chain is wrapped in a group
- **THEN** the node count includes the group, the evaluated count does not, and consolidation is still not advised
