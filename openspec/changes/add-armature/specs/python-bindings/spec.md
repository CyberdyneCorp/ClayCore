# python-bindings — armatures

Delta for `add-armature`.

## ADDED Requirements

### Requirement: Armatures from Python
`pyclay` SHALL expose armatures with the same semantics as the C ABI, taking nodes as an (N, 4) array of position and radius plus an (N,) array of parent indices, matching how strokes and sweeps already take their points.

`check_binding_parity` SHALL report no capability without a C counterpart.

#### Scenario: Both bindings agree
- **WHEN** the same armature is built through the C ABI and through pyclay
- **THEN** the two documents evaluate identically
