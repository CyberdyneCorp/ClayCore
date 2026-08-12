# c-abi — a sign per armature node

Delta for `add-armature-node-signs` (#99). Builds on the `read-armature-tree`
delta's text (shipped, unarchived): archive that change first, then this one.

## MODIFIED Requirements

### Requirement: Armatures across the ABI
The C API SHALL expose building an armature item from nodes and their parents, and the tree edits, and SHALL be purely additive: no existing signature changes and no struct grows.

Each node SHALL carry a sign, +1 or -1, positive by default, set beside the parents by a signs setter that mirrors the parents setter — one entry per node — and any value other than +1 or -1 SHALL be refused as a typed invalid argument, because the negative-radius convention would legalise input the point setter refuses today. A fifth tree edit SHALL set a placed node's sign, carrying it in the existing radius argument, so no signature changes shape; it SHALL be one undoable command and SHALL be refused on a protected layer, exactly as the other four edits are. A negative node SHALL NOT be required to be a leaf.

Reading a placed armature's tree back SHALL be split the way the setters are split, because an armature IS a stroke plus a tree: the point readback SHALL accept the armature primitive and serve its nodes as the same x, y, z, radius list it serves for strokes and guides, and a parents readback of its own SHALL serve one parent index per node, a root naming itself, by the same size-query pattern — a null buffer answers with the count, an undersized buffer yields a too-small error carrying the needed count and writes nothing. A signs readback SHALL serve one sign per node by the same pattern, and every readback SHALL agree on the count: both are counted in nodes, a tree stored with fewer parents than nodes reads back padded with roots, and signs stored shorter than nodes read back padded positive — exactly the reading compilation makes, so what comes back is the tree the document evaluates. The parent indices SHALL be the indices the tree edits take, and a node that is not an armature SHALL be refused the parents and signs readbacks with a typed invalid-argument error. Reading SHALL NOT be refused on a protected or hidden layer, because protection refuses edits.

Replacing a placed armature's points through the curve replace SHALL remain refused: points replaced alone would desynchronise from the parents, and the tree edits own that half.

#### Scenario: An armature round trips through a host
- **WHEN** a host reads a placed armature's nodes and parents, moves one node, and writes the tree back
- **THEN** the document reflects the move, with the subtree carried

#### Scenario: A malformed tree is refused
- **WHEN** an armature is built with a parent index outside the node range, or with a cycle
- **THEN** it is refused with the reason, and the document is unchanged

#### Scenario: A reloaded branching rig is re-posable
- **WHEN** a host saves a document holding an armature whose tree branches, reloads it, reads both halves back, and moves a node through the tree edits using the read-back indices
- **THEN** the nodes and parents match what was authored, including the branch, and the move carries exactly the subtree the read-back tree names

#### Scenario: The parents readback keeps the refusals typed
- **WHEN** the parents of a stroke, a group or a missing node are asked for, or a buffer smaller than the node count is passed
- **THEN** the stroke and the group are refused as invalid arguments, the missing node as not found, and the short buffer with the too-small error carrying the needed count and nothing written

#### Scenario: A negative node survives the round trip
- **WHEN** a host sets a sign array with one negative node, saves, reloads, and reads the signs back
- **THEN** the signs match what was authored, and flipping the node positive through the sign edit restores the all-positive field

#### Scenario: The signs surface keeps the refusals typed
- **WHEN** a sign of 0 or ±2 is passed to the setter or the sign edit, the signs of a stroke are asked for, or a short buffer is passed to the signs readback
- **THEN** each is refused with its typed error — invalid argument for the values and the stroke, too-small carrying the needed count for the buffer — and nothing is written

#### Scenario: A negative node carries children
- **WHEN** a node with descendants is set negative
- **THEN** the edit succeeds, the descendants keep their own signs, and moving the negative node still carries its subtree
