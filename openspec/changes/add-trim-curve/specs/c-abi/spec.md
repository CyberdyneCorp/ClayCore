# c-abi — Trim Curve

Delta for `add-trim-curve`.

## ADDED Requirements

### Requirement: Flattening a trim curve across the C ABI
The ABI SHALL expose flattening an OPEN control-point curve into a trim outline, beside the closed-lasso flattener it already has, following the same size-query convention. An unknown side, a tolerance that is not positive, or fewer points than describe a stroke SHALL be refused.

#### Scenario: A host flattens a trim
- **WHEN** a host flattens an open curve for a side
- **THEN** it receives an outline whose closing edge runs along the frame bound on that side

#### Scenario: The other side closes the other way
- **WHEN** the same curve is flattened for the opposite side
- **THEN** the closing edge runs along the opposite bound
