# c-abi — the Tube tool

Delta for `add-tube-tool`.

## ADDED Requirements

### Requirement: Resolving a tube across the C ABI
The ABI SHALL expose resolving a path into a tube, with its settings in a versioned descriptor struct, returning an ordinary item the caller owns. A parametric profile SHALL select the swept representation and its absence the exact swept-sphere one, as it does elsewhere.

Fewer points than describe a path, a radius positive nowhere, an unknown point type, or a polygon profile SHALL be refused rather than yielding an item that contributes nothing.

#### Scenario: A round tube stays exact across the boundary
- **WHEN** a host resolves a tube with no profile and adds it to a document
- **THEN** the document's safe step scale is 1

#### Scenario: A profiled tube declares its cost
- **WHEN** the same path is resolved with a box profile
- **THEN** the safe step scale is below 1

#### Scenario: Degenerate input is refused
- **WHEN** a tube is asked for from one point, or with every radius zero
- **THEN** no item is returned
