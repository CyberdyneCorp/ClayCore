# c-abi — armatures

Delta for `add-armature`.

## ADDED Requirements

### Requirement: Armatures across the ABI
The C API SHALL expose building an armature item from nodes and their parents, and the tree edits, and SHALL be purely additive: no existing signature changes and no struct grows.

Reading a placed armature's tree back SHALL follow the size-query pattern the other variable-length readbacks use, so a host that reloads a document can edit an armature it did not author — the gap that `clay_layer_stroke_points` closed for curves.

#### Scenario: An armature round trips through a host
- **WHEN** a host reads a placed armature's nodes and parents, moves one node, and writes the tree back
- **THEN** the document reflects the move, with the subtree carried

#### Scenario: A malformed tree is refused
- **WHEN** an armature is built with a parent index outside the node range, or with a cycle
- **THEN** it is refused with the reason, and the document is unchanged
