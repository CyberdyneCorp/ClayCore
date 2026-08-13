# python-bindings — a sign per armature node

Delta for `add-armature-node-signs` (#99).

## MODIFIED Requirements

### Requirement: Armatures from Python
`pyclay` SHALL expose armatures with the same semantics as the C ABI, taking nodes as an (N, 4) array of position and radius plus an (N,) array of parent indices, matching how strokes and sweeps already take their points, and an optional (N,) array of signs, +1 or -1, absent meaning all positive. The signs SHALL be builder state readable and writable as a property, the placed sign edit SHALL be reachable through the armature edit call, and any sign other than +1 or -1 SHALL be refused.

`check_binding_parity` SHALL report no capability without a C counterpart.

#### Scenario: Both bindings agree
- **WHEN** the same armature is built through the C ABI and through pyclay
- **THEN** the two documents evaluate identically

#### Scenario: A negative node from Python
- **WHEN** a script builds an armature with one node's sign -1 and the same rig through the C ABI signs setter
- **THEN** the two documents evaluate identically, and the hollow is present in both

#### Scenario: The example carves
- **WHEN** the armature example runs
- **THEN** it builds a rig holding negative nodes and renders the carved result alongside the all-positive rig
