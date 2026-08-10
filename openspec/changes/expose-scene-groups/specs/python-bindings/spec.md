# python-bindings — expose scene groups

Delta for `expose-scene-groups`.

## ADDED Requirements

### Requirement: Groups in pyclay
`pyclay` SHALL expose the group surface with the same semantics as the C ABI, so that `check_binding_parity` reports no capability without a C counterpart.

The name SHALL NOT collide with undo grouping, which is a different concept already spelled `begin_undo_group` / `end_undo_group`.

Placing an edit inside a group SHALL be an argument on the existing add rather than a second add: pyclay takes keyword arguments, and a second entry point would be a second way to say the same thing.

#### Scenario: Both bindings agree
- **WHEN** the same grouped construction is built through the C ABI and through pyclay
- **THEN** the two documents evaluate identically

#### Scenario: Children read back
- **WHEN** a script asks a layer for a group's children
- **THEN** it receives the child ids in order, and asking an item is a `ValueError`

#### Scenario: The refusals are the C ABI's refusals
- **WHEN** a script gives a group a transition op, gives an inline group a blend or rounding, sets a transform on a group, or moves a node into its own subtree
- **THEN** each is a `ValueError` and the document is unchanged
