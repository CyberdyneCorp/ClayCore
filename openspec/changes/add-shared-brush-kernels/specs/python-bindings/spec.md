# python-bindings — brush presets

Delta for `add-shared-brush-kernels`.

## ADDED Requirements

### Requirement: The brush model is reachable from Python
`pyclay` SHALL expose the brush model and preset surface the C ABI exposes, so that the binding-parity gate passes rather than recording an exemption.

Arrays SHALL be numpy-native where a sequence is natural, and a preset SHALL serialize to and from `bytes`.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every brush-model capability reachable from the C ABI is reachable from `pyclay`
