# python-bindings — one-sided flatten

Delta for `add-flatten-modes`.

## ADDED Requirements

### Requirement: Choosing a flatten mode from Python
The module SHALL expose the flatten mode, defaulting to two-sided, and SHALL name the vendor brushes the one-sided modes correspond to so a caller can find them.

#### Scenario: A script polishes without filling
- **WHEN** a script flattens in cut-only mode over a surface with a hollow in it
- **THEN** the hollow is untouched and the high material is planed off
