# build-packaging — the kernels and the tape are versioned together

Delta for `add-tape-abi-export`.

## ADDED Requirements

### Requirement: The kernel package and the tape encoding share a version
The published kernels artifact and the tape encoding a consumer feeds them SHALL carry one version. A host compiles the published headers and feeds them an exported tape; those two only work together, so they SHALL NOT be versioned independently.

The host parity fixture SHALL cover the exported-tape path, not only the fixture's own bundled tapes: a consumer's evaluator agreeing on a fixture and disagreeing on a live document is the failure this fixture exists to prevent.

#### Scenario: The package states the tape version it expects
- **WHEN** the kernels artifact is built
- **THEN** it records the tape encoding version its headers evaluate

#### Scenario: The fixture covers a live tape
- **WHEN** the parity fixture is exercised
- **THEN** it includes a tape obtained through the export path, evaluated by the published headers, and gates it at the same tolerance
