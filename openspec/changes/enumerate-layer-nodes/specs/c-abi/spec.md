# c-abi — enumerate a layer's nodes

Delta for `enumerate-layer-nodes` (#91).

## ADDED Requirements

### Requirement: A host can discover a layer's nodes
The C API SHALL let a host enumerate the nodes a layer holds by a count and an index, where the index is the layer's EVALUATION order, mirroring the layer-level pair the discovery requirement above defines. An index at or beyond the count SHALL be a typed not-found, so a host walks to the end without a sentinel, and a layer id that names no layer SHALL be a typed not-found as well.

The enumeration SHALL cover the layer's TOP-LEVEL nodes only, and the header SHALL say so plainly: it is the sibling of the group-children query, which continues to descend, so the whole tree is walked by pairing the two — enumerate the layer's roots, ask the node-primitive query what each one is, and recurse through the ones it refuses as groups. A layer's root SHALL NOT be addressable as a node id, because id 0 is the no-node sentinel and a call that answered for it could no longer refuse an id that does not exist.

Enumeration SHALL go through the index rather than the id space, because node ids are not dense and nothing bounds the length of a gap left by a removal: probing ids upward against the node-primitive query loses every node past the longest run of misses the caller happened to tolerate. A layer that carries no SDF content — a voxel or a mesh layer — SHALL count zero rather than fail, the same reading the per-layer field evaluation makes of it. Reading is not editing: a ghosted, locked or hidden layer SHALL answer normally. The addition SHALL be purely additive — no existing signature changes and no existing call's meaning moves.

#### Scenario: A reloaded document finds its armature without probing
- **WHEN** a document whose layer holds an item, a group and an armature placed after a run of removed nodes is saved, reloaded, and walked by enumerating layers, then enumerating that layer's nodes, then asking each node which primitive it carries
- **THEN** the armature is found without probing any node id and without a tolerance for missing ids, and its points and its parents read back exactly as authored

#### Scenario: Enumeration is top level, children descends
- **WHEN** a layer holds a loose item and a group containing two items, and the layer's nodes are enumerated
- **THEN** exactly the loose item and the group are reported, the group's items are not, and the group's items are reached by the group-children query on the enumerated group id

#### Scenario: Removed nodes leave gaps the index steps over
- **WHEN** several consecutive nodes are removed and a node is added after them
- **THEN** enumeration reports the surviving nodes and only those, in evaluation order, while each removed id is refused by the node-primitive query

#### Scenario: The enumerators keep their refusals typed
- **WHEN** the count or index query is given an id that is not a layer's, an index at or past the count, or a null out-parameter
- **THEN** the first two are refused as not found and the last as an invalid argument, with nothing written
