# python-bindings — expose scene groups

Delta for `expose-scene-groups`.

## ADDED Requirements

### Requirement: Groups in pyclay
`pyclay` SHALL expose the group surface with the same semantics as the C ABI, so that `check_binding_parity` reports no capability without a C counterpart.

The name SHALL NOT collide with undo grouping, which is a different concept already spelled `begin_undo_group` / `end_undo_group`.

#### Scenario: Both bindings agree
- **WHEN** the same grouped construction is built through the C ABI and through pyclay
- **THEN** the two documents evaluate identically
