## ADDED Requirements

### Requirement: The consolidation advice reaches pyclay

`Layer.consolidation_advice` SHALL take the caller's step-scale tolerance and
report whether consolidating is advised, the suggested consolidation parameters,
and the cost projected at those parameters — the same three answers the C ABI
gives, so a script can exercise the property that the advised parameters cure
the degradation the field report named.

When the bake is not advised, the parameters and the cost SHALL be `None` rather
than a zeroed object. That is the Python form of the C surface's zeroed
descriptor and it fails in the same direction: passing `None` to
`Layer.consolidate` raises at the call rather than baking something.

The binding SHALL NOT invent, default or store a cell size of its own. A script
that never calls this SHALL still be required to supply one.

#### Scenario: The advice cures what the report named
- **WHEN** a script reports a degraded layer, consolidates it with the advised parameters, and reports it again
- **THEN** the second report advises nothing, names no degradation, and its safe step scale reaches the threshold

#### Scenario: Not advised is None, not zeroes
- **WHEN** a layer is not advised
- **THEN** the parameters and cost are `None`, and passing them to `Layer.consolidate` raises rather than baking

#### Scenario: A brush chain is not advised from Python
- **WHEN** a layer of one item carrying a deep grab is queried below the caller's threshold
- **THEN** `advises` is False, matching the `degradation` of "deformers" the field report gives for it

#### Scenario: Asking changes nothing
- **WHEN** a script asks for the advice
- **THEN** the layer's item count, its field and its consolidation state are what they were
