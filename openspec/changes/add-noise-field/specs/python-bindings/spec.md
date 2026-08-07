# python-bindings — a noise field

Delta for `add-noise-field`.

## ADDED Requirements

### Requirement: Noise from Python
The module SHALL expose the noise deformer on items alongside the others, with the amplitude, frequency, octaves, gain and seed under the caller's control.

The binding SHALL state that the seed is an ordinary parameter rather than global state, so two items with the same seed look the same and an item's appearance does not depend on the order it was compiled in.

#### Scenario: A script roughens a shape
- **WHEN** a script adds the noise deformer to an item
- **THEN** the document's field shows an irregular surface

#### Scenario: The seed is reproducible
- **WHEN** the same script runs twice
- **THEN** it produces the same field both times
