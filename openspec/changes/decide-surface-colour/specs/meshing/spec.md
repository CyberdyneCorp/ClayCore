# meshing — surface colour

Delta for `decide-surface-colour`.

## ADDED Requirements

### Requirement: A mesh brush does not disturb vertex colours
The fixed-topology mesh verbs move vertices and SHALL leave `colors` untouched, so an imported model's colours survive sculpting on it.

Stated as a requirement rather than left implicit because it is currently true by omission — no verb writes colour — and a future colour brush must add colour writing deliberately rather than by accident.

#### Scenario: Sculpting a coloured mesh keeps its colours
- **WHEN** any mesh verb is stamped on a mesh carrying vertex colours
- **THEN** `colors` is byte-identical before and after
