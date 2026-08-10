# c-abi — a consolidation policy

Delta for `add-consolidation-policy`.

## ADDED Requirements

### Requirement: Consolidation across the ABI
The C API SHALL expose consolidating a layer and reporting its cost before it is paid, reusing what a volume already reports — bytes, brick count, sample count and sample Lipschitz. The addition SHALL be purely additive.

The cost SHALL also carry what the sample Lipschitz IMPLIES — the declared
Lipschitz and the safe step scale — because those are the numbers a host budgets
a frame against, and deriving them from `sqrt(3) x max(l, 1)` in every binding
would be re-implementing a kernel combinator outside the kernel.

#### Scenario: The cost is knowable before consolidating
- **WHEN** a host asks what consolidating a layer would cost
- **THEN** it gets the memory and resolution it would spend, without the document changing

#### Scenario: The quote is the bill
- **WHEN** a host quotes a consolidation and then performs it with the same parameters
- **THEN** the brick count and byte count it was quoted are the ones it pays

### Requirement: The trigger is advisory across the ABI
The C API SHALL let a host measure a layer's degradation and SHALL NOT consolidate on its own. The threshold that turns a measurement into advice SHALL be an argument of the query rather than document state, because a tolerance for marching cost belongs to a viewport, a device and a frame budget rather than to the artwork — and storing it would need a document format bump to carry it.

#### Scenario: A host is told, and decides
- **WHEN** a host asks for a layer's field report with a step-scale threshold
- **THEN** it is told whether the layer has degraded past that threshold, and nothing is baked

#### Scenario: A measurement without a threshold makes no recommendation
- **WHEN** a host asks for a field report with a threshold of zero
- **THEN** it gets the numbers and no advice
