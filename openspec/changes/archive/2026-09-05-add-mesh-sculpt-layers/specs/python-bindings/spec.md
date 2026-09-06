# python-bindings — sculpt layers

Delta for `add-mesh-sculpt-layers`.

## ADDED Requirements

### Requirement: The layer stack is reachable from Python
`pyclay` SHALL expose the sculpt layer stack, its property operations, the stroke transaction and the high-detail stamp modes, so the binding-parity gate passes rather than recording an exemption.

A stroke transaction SHALL be available as a context manager, following the voxel sculpt layer's precedent — it is the only form that cannot leave a surface recording when a stroke loop raises.

Image inputs SHALL be numpy arrays borrowed for the call.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every sculpt-layer capability reachable from the C ABI is reachable from `pyclay`

#### Scenario: A raising stroke loop leaves nothing recording
- **WHEN** an exception is raised inside a stroke-transaction context manager
- **THEN** the transaction is cancelled and the layer is unchanged
