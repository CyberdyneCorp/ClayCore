# scene-model — what a group means, stated from outside

Delta for `expose-scene-groups`.

Groups have compiled since the first version and nothing outside the engine
could build one, so nothing outside the engine pinned what one means. These
requirements state the behaviour `compile_group`, `node_influence_bound` and
`SdfContent::move` already have to have, now that a host can reach them.

## ADDED Requirements

### Requirement: A group is a sub-expression
A group's children SHALL compile against a fresh accumulator and combine with the chain outside the group as one value, through the group's own op, blend and rounding. An op inside a group therefore SHALL NOT reach anything outside it.

A group SHALL have no transform of its own: the compiler composes `layer.xform * item.xform` and nothing else, so a transform on a group would change nothing. The bindings SHALL refuse to record one rather than accept an edit that is undoable and saved but inert. For the same reason a group's rounding SHALL scale by the layer's scale alone, where an item's scales by the layer's and its own.

A group SHALL NOT carry a transition op: the compiler emits no transition parameters for a group, so one would morph on defaults the node never stated.

#### Scenario: An intersect stays inside its group
- **WHEN** a group holds a shell and a cutter combined with intersect, and the layer already holds other geometry
- **THEN** the intersect trims the shell only, and the group's result combines with the rest through the group's own op

#### Scenario: A group's transform is refused rather than ignored
- **WHEN** a host sets a transform on a group
- **THEN** the edit is refused and the document is unchanged

### Requirement: An inline group is its children, exactly
A group carrying the inline op SHALL compile its children against the OUTER accumulator, so the field is bit-identical to the same children added directly in the same order. Its own blend, rounding and colour are never read, and the bindings SHALL refuse them rather than accept values that cannot take effect.

#### Scenario: Inline and flat agree bit for bit
- **WHEN** the same ordered edits are compiled once under an inline group and once at the layer root
- **THEN** the two fields are identical at every sampled point, not merely close

### Requirement: A group with nothing to combine emits nothing
A group whose op carves (subtract, intersect, the extended modes other than the material-creating ones) and that has no accumulated value beneath it SHALL emit no instructions, as a carving item in the same position does. A group whose children all turn out to be invisible or culled SHALL likewise emit nothing: any partial emission SHALL be rolled back, so the tape is identical to one compiled without the group at all.

#### Scenario: A carving group first in a chain
- **WHEN** a subtract group is the first node in a layer
- **THEN** the field is the empty field, not a hole in it

#### Scenario: A group whose subtree compiled to nothing
- **WHEN** a group's children are all hidden or culled
- **THEN** the compiled tape and the evaluated field are identical to the document without that group

### Requirement: A node cannot become its own descendant
Reparenting SHALL refuse to move a node into its own subtree, and SHALL leave the tree untouched when it refuses. A move that closed such a cycle would detach the subtree from the root list, so it would stop evaluating, be dropped by the next save (serialization walks from the roots) and be unreachable by removal.

#### Scenario: A group moved under its own descendant
- **WHEN** a group is moved into one of its own children or grandchildren
- **THEN** the move fails, the node keeps its parent and index, and the subtree still evaluates

### Requirement: A group's influence bound covers its own combine
A group's influence bound SHALL be the union of its children's bounds dilated by what the group's own combine reaches — for an extended op its documented support, and otherwise the greater of the blend profile's support and the blend radius, which is what the item path already uses. A hard profile supports zero distance while a paint combine still fades over the radius, so the profile's support alone is not conservative.

#### Scenario: A paint group with a hard profile
- **WHEN** a group combines with paint, a hard profile and a non-zero radius
- **THEN** its influence bound is dilated by at least that radius

## MODIFIED Requirements

### Requirement: Undo command vocabulary
Every document mutation SHALL be expressed as a serializable command with a computable inverse: add/remove/reorder item, set parameter, voxel-span edit, layer add/remove/reorder/retransform. Grouping is not a command of its own: a group is a node, so creating one is an add, deleting one is a remove that carries the whole subtree back, and grouping N existing siblings is N reparents bracketed into a single undo step. The in-memory undo stack and the document file format SHALL share this single command vocabulary. Consecutive commands from one stroke SHALL be coalescable into a single undo step. Item state carried by commands SHALL include any deformer chain, so deformed documents round-trip.

The undo stack SHALL be reachable from the bindings, so a host application uses the engine's undo rather than reimplementing one over a second vocabulary that could disagree with what a saved document records.

#### Scenario: Command inverse restores state
- **WHEN** any command from the vocabulary is applied to a document and then its inverse is applied
- **THEN** the document state is bit-identical to the original (verified by serialization comparison)

#### Scenario: Stroke coalescing
- **WHEN** a sculpt stroke generates N incremental point-append commands followed by stroke end
- **THEN** undo removes the entire stroke as one step

#### Scenario: Deformed item round trip
- **WHEN** a document containing an item with a deformer chain is serialized and reloaded
- **THEN** the reloaded document evaluates bit-identically and re-serializes to identical bytes

#### Scenario: A host application undoes through the engine
- **WHEN** a binding performs an edit on a document with undo enabled and then undoes it
- **THEN** the document serializes bit-identically to its state before the edit

#### Scenario: An edit to a group undoes exactly
- **WHEN** a group's op is changed, a child is added to it, it is reparented, or the whole group is removed, on a document with undo enabled
- **THEN** one undo restores the document to bit-identical bytes
