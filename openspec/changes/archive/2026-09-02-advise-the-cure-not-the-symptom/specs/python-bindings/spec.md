## ADDED Requirements

### Requirement: The field report names its mechanism from Python
`Layer.field_report` SHALL report `steepest_deformer_chain`, `drawable_count` and `degradation` — the last as one of "none", "volumes", "deformers", "both" — alongside what it already returns, and `advises_consolidation` SHALL follow the mechanism.

#### Scenario: A brush chain on one item is not advised
- **WHEN** a layer of one item carrying a deep grab is reported below the caller's threshold
- **THEN** `degradation` is "deformers" and `advises_consolidation` is False

#### Scenario: The same chain over an edit list is advised
- **WHEN** the same grab is applied to a layer of twenty items
- **THEN** `degradation` is "both" and `advises_consolidation` is True
