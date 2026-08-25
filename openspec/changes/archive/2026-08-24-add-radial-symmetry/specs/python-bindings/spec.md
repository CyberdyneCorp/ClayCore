# python-bindings

## ADDED Requirements

### Requirement: A layer's radial symmetry is reachable from Python
`pyclay` SHALL expose a layer's radial symmetry with the same reach as the C ABI, following the shape of `Layer.mirror`: an axis named by string, a count, and a seam blend. Setting a count below 2 SHALL clear the mode.

#### Scenario: Python and the C ABI agree
- **WHEN** the binding-parity check runs
- **THEN** the radial entry point has both a Python and a C counterpart, and neither is exempted
