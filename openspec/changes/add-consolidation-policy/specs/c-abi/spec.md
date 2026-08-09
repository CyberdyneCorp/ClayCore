# c-abi — a consolidation policy

Delta for `add-consolidation-policy`.

## ADDED Requirements

### Requirement: Consolidation across the ABI
The C API SHALL expose consolidating a region and reporting its cost before it is paid, reusing what a volume already reports — bytes, brick count, sample count and sample Lipschitz. The addition SHALL be purely additive.

#### Scenario: The cost is knowable before consolidating
- **WHEN** a host asks what consolidating a region would cost
- **THEN** it gets the memory and resolution it would spend, without the document changing
