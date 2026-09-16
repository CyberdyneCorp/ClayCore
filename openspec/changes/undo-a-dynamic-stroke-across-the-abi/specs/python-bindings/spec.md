## ADDED Requirements

### Requirement: Adaptive-surface undo from Python
The module SHALL expose a `TopologyDelta` class with `revert(sculptor)`, `apply(sculptor)`, `clear()`, a `stats` mapping carrying the same fields as the C statistics struct, `serialize()` and a static `deserialize(bytes)`. `DynamicSculptor.stamp` SHALL accept a `record` argument that accumulates into a `TopologyDelta`. The binding parity gate SHALL map the class to the `clay_dynamic_delta_` calls and `record` to `clay_dynamic_sculptor_stamp_recorded`.

A replay the engine refuses SHALL raise, and SHALL leave the surface unchanged.

#### Scenario: Undo and redo from Python match the exports
- **WHEN** a script records an adaptive stroke, reverts it, and applies it again
- **THEN** `to_mesh` equals the pre-stroke export after the revert and the post-stroke export after the apply, and `validate()` passes at both ends

#### Scenario: A refused replay raises
- **WHEN** a script reverts an older record while a newer one is applied
- **THEN** the call raises, and the surface's revisions are unchanged
