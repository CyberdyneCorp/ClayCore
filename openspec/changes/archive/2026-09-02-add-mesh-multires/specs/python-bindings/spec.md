# python-bindings — multiresolution surfaces

Delta for `add-mesh-multires`.

## ADDED Requirements

### Requirement: The hierarchy is reachable from Python
`pyclay` SHALL expose level creation and removal, the sculpt and display levels, sculpting at a level, export at a level, and the memory accounting, so the binding-parity gate passes rather than recording an exemption.

Arrays SHALL be numpy-native, and an exported level SHALL return the same arrays the existing mesh readback returns.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every multiresolution capability reachable from the C ABI is reachable from `pyclay`
