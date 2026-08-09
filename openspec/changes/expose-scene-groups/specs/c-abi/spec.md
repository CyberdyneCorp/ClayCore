# c-abi — expose scene groups

Delta for `expose-scene-groups`.

## ADDED Requirements

### Requirement: Groups across the ABI
The C API SHALL let a host create a group node in a layer, give it an op and a blend as for any other node, and add children to it including nested groups. A group SHALL be identified by a node id, as every other node is.

This exposes behaviour the scene model already implements; it does not introduce a new evaluation concept. The addition SHALL be purely additive.

#### Scenario: A sub-expression is expressible from a host
- **WHEN** a host builds a group containing a shell and an intersecting cutter, and combines that group into a layer that already holds other geometry
- **THEN** the intersect applies only within the group, and the group's result combines with the rest through the group's own op

#### Scenario: An inline group applies to the outer chain
- **WHEN** a group is created with the inline op and given children
- **THEN** its children combine into the outer chain exactly as if they had been added directly

#### Scenario: A group round trips
- **WHEN** a document containing nested groups is saved and reloaded
- **THEN** it evaluates bit-identically and reserialises to identical bytes
