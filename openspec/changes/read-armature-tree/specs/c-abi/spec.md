# c-abi — read a placed armature's tree back

Delta for `read-armature-tree` (#77).

## MODIFIED Requirements

### Requirement: Armatures across the ABI
The C API SHALL expose building an armature item from nodes and their parents, and the tree edits, and SHALL be purely additive: no existing signature changes and no struct grows.

Reading a placed armature's tree back SHALL be split the way the setters are split, because an armature IS a stroke plus a tree: the point readback SHALL accept the armature primitive and serve its nodes as the same x, y, z, radius list it serves for strokes and guides, and a parents readback of its own SHALL serve one parent index per node, a root naming itself, by the same size-query pattern — a null buffer answers with the count, an undersized buffer yields a too-small error carrying the needed count and writes nothing. The two halves SHALL agree on the count: the parents readback is counted in nodes, and a tree stored with fewer parents than nodes reads back padded with roots, exactly the reading compilation makes of it, so what comes back is the tree the document evaluates. The parent indices SHALL be the indices the tree edits take, and a node that is not an armature SHALL be refused the parents readback with a typed invalid-argument error. Reading SHALL NOT be refused on a protected or hidden layer, because protection refuses edits.

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

## ADDED Requirements

### Requirement: A placed node answers what primitive it carries
The C API SHALL let a host ask a placed item which primitive it carries, returning the same enumeration item creation takes, so that a host that reloaded a document picks the reader that applies instead of probing readers until one stops refusing. A group SHALL be refused with a typed invalid-argument error — the dual of the children query refusing an item — so every node answers exactly one of the two questions. Reading SHALL NOT be refused on a protected or hidden layer.

#### Scenario: A reloading host finds its armature
- **WHEN** a host asks a placed armature node and a placed sphere node what they carry
- **THEN** it receives the armature and sphere enumerators it would have passed to create them

#### Scenario: A group is the other question
- **WHEN** the primitive of a group is asked for, and the children of an item
- **THEN** both are refused as invalid arguments, and each node answers exactly one of the two queries
