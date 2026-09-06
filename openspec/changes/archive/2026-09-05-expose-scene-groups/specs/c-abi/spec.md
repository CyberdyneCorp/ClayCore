# c-abi — expose scene groups

Delta for `expose-scene-groups`.

## ADDED Requirements

### Requirement: Groups across the ABI
The C API SHALL let a host create a group node in a layer, give it an op and a blend as for any other node, and add children to it including nested groups. A group SHALL be identified by a node id, as every other node is, and SHALL be edited, moved, removed and undone through the entry points that already take a node id.

The existing add entry points append to the layer root and have no argument that could say otherwise, so the ABI SHALL carry a parented form of each rather than requiring an add followed by a move — which would be two undo steps for one intention.

This exposes behaviour the scene model already implements; it does not introduce a new evaluation concept. The addition SHALL be purely additive: no existing signature changes, no struct grows, no enumerator's value changes.

#### Scenario: A sub-expression is expressible from a host
- **WHEN** a host builds a group containing a shell and an intersecting cutter, and combines that group into a layer that already holds other geometry
- **THEN** the intersect applies only within the group, and the group's result combines with the rest through the group's own op

#### Scenario: An inline group applies to the outer chain
- **WHEN** a group is created with the inline op and given children
- **THEN** its children combine into the outer chain exactly as if they had been added directly

#### Scenario: A group round trips
- **WHEN** a document containing nested groups is saved and reloaded
- **THEN** it evaluates bit-identically and reserialises to identical bytes

### Requirement: A host can enumerate a group's children
The ABI SHALL report a group's children, in order, by the size-query pattern the rest of the ABI uses: a null buffer answers with the count, a buffer of that size is filled, and a buffer that is too small reports the needed count and writes nothing. A node that is not a group SHALL be an invalid argument rather than an empty answer, since that refusal is also how a host that reloaded a document tells a group from an item.

Reading is not editing, so a ghosted, locked or hidden layer SHALL answer normally.

#### Scenario: Count, then fill
- **WHEN** a host queries a group's children with a null buffer and then with a buffer of the reported size
- **THEN** it receives the child count and then the child ids in tree order

#### Scenario: An item answers that it is not a group
- **WHEN** the query names an item
- **THEN** it is refused as an invalid argument, and a node the layer does not hold is not found

### Requirement: The inline op is groups only
The header SHALL declare the inline op as a `clay_op` enumerator, because the value appears in saved documents and in the tape and a host that met it would otherwise be staring at an undocumented number. It SHALL be accepted on a group and refused on an item, exactly as the engine's own predicate already refuses it.

An inline group reads no blend, rounding or colour, so the ABI SHALL refuse those rather than accept values that cannot take effect — an accepted blend would still dilate the group's influence bound and dirty more than the edit touches.

#### Scenario: An item refuses the inline op
- **WHEN** a host sets the inline op on an item
- **THEN** the edit is refused and the document is unchanged

#### Scenario: An inline group refuses a blend
- **WHEN** a group is created with the inline op and a non-zero blend radius or rounding
- **THEN** the call is refused
