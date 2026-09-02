## ADDED Requirements

### Requirement: The field report names the mechanism and is advised on it
`clay_field_report` SHALL carry the deformer mechanism's own factor, the count of nodes that are evaluated, and a `clay_degradation` naming which mechanism is costing the marcher. `advises_consolidation` SHALL be keyed on that mechanism rather than on the step scale alone.

The fields SHALL be appended behind `struct_size`, so a caller built against the earlier struct keeps working and nothing is written past the end of the struct it owns.

#### Scenario: An older caller is unaffected
- **WHEN** clay_layer_field_report is called with the struct_size of the earlier struct
- **THEN** it succeeds, fills the fields that struct has, and writes nothing past its end

#### Scenario: The advice follows the mechanism
- **WHEN** a degraded layer holds one evaluated item carrying a brush chain
- **THEN** `degradation` is CLAY_DEGRADATION_DEFORMERS and `advises_consolidation` is 0
