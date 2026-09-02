# python-bindings — adaptive surfaces

Delta for `add-dynamic-topology`.

## ADDED Requirements

### Requirement: Adaptive sculpting is reachable from Python
`pyclay` SHALL expose the adaptive surface, its sculptor, its topology settings and its conversion to and from a mesh, so the binding-parity gate passes rather than recording an exemption.

Buffers SHALL be numpy-native, and the conversion to a mesh SHALL return the same arrays the existing mesh readback returns.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every adaptive-surface capability reachable from the C ABI is reachable from `pyclay`
