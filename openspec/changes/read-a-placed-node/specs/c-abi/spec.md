# c-abi — read a placed node back

Delta for `read-a-placed-node`.

## ADDED Requirements

### Requirement: A host can read a placed node's state back
The C API SHALL report a placed node's transform, its primitive's parameters and its op, blend and rounding, completing the reading half of the four setters that write them. Until this existed the only accessor on a node reported WHICH primitive it carried and nothing else, so a host that placed a primitive, moved it with a manipulator and edited its operation afterwards had to keep those values itself — in a second file beside the document, keyed by node id, whose correctness across undo, redo and reload it also had to maintain.

Each reader SHALL take the arguments its setter takes, so that what comes out goes straight back in without a conversion the caller has to get right. Every out-pointer SHALL be optional, and a call passing none of them SHALL still validate the layer and the node, which is how a host asks whether an id is still a node of that layer.

The influence bound SHALL NOT be treated as an answer to any of these questions, and the header SHALL say why: it is dilated by rounding and blend support, and under a layer mirror it covers the reflection too, so an item placed at x = 0.9 in a mirrored layer reports a bound centred on the origin.

Reading is not editing: a ghosted, locked or hidden layer SHALL answer normally. The addition SHALL be purely additive — no existing signature changes, no struct grows, no enumerator moves, and nothing new is stored, so no document or scene format version moves either.

#### Scenario: A reloaded document reports placement without a side-car
- **WHEN** a document whose layer is mirrored, hidden, ghosted and locked is saved with a placed primitive, reloaded, and walked by enumerating its layers, then its nodes, then asking each node which primitive it carries
- **THEN** that node's position, rotation, scale, parameters, op, blend and rounding all read back as authored, while the influence bound of the same node reports a box centred on the origin and wider than the primitive

#### Scenario: Every out-pointer is optional
- **WHEN** a reader is called with some or all of its out-pointers null
- **THEN** it writes only the values asked for and still refuses an id the layer does not hold

### Requirement: A placed node's transform reads back as its setter takes it
The transform query SHALL report position, a rotation axis, a rotation angle and a scale — the four values the transform setter takes. A GROUP SHALL be refused as an invalid argument, for the reason its setter refuses one: the compiler composes layer and item and nothing else, so a group holds no transform to answer with.

The node stores a rotation as a quaternion, so the axis and angle SHALL be A representative of that rotation rather than the exact pair last written, and the representative SHALL be canonical: the angle SHALL come back in `[0, pi]`, with the axis flipped where that is what naming the same rotation inside that range takes.

The axis SHALL always be unit length and SHALL NEVER be zero: an unrotated item SHALL read back as angle 0 about a named axis, because the transform setter refuses a zero axis and a reader whose output its own setter rejects would not be a round trip. Applying what the reader returned and reading again SHALL give the same values.

#### Scenario: A placement round trips
- **WHEN** an item is placed at a position with a non-unit rotation axis, an angle and a scale, read back, placed again with exactly what was read, and read a second time
- **THEN** the second reading equals the first, and the axis reported is the normalized one

#### Scenario: A turn past pi is named inside the range
- **WHEN** an item is rotated by an angle greater than pi about an axis and read back
- **THEN** the angle reported is inside `[0, pi]` about the flipped axis, and re-applying it is a fixed point

#### Scenario: An unrotated item still names an axis
- **WHEN** an item carrying no rotation is read back
- **THEN** the angle is 0 and the axis is unit length rather than zero, so the values are accepted by the transform setter

#### Scenario: A group has no transform to report
- **WHEN** the transform query names a group
- **THEN** it is refused as an invalid argument and nothing is written

### Requirement: A placed node's parameters read back by the size-query pattern
The parameter query SHALL follow the size-query pattern the rest of the reading surface uses, counted in FLOATS: a null buffer answers with the count, a buffer of that size is filled, and a buffer that is too small reports the needed count and writes nothing.

The count SHALL be the arity of the primitive the node CURRENTLY carries, so that a caller that has just learned the primitive from the node-primitive query can size a buffer without carrying a table of arities of its own, and so that replacing a primitive changes the count as well as the values. A primitive whose payload is out of line — a stroke, an armature, a sampled volume — SHALL count zero rather than refuse, so a walk that asks every node for its parameters needs no special case for the kinds whose payload is read by a typed reader instead. A lift or a loft SHALL report its own parameters and not its profiles, exactly as its setter takes them.

A group carries no primitive and SHALL be refused as an invalid argument, as the node-primitive query already refuses one.

#### Scenario: Count, then fill, then set it back
- **WHEN** a host queries a placed box's parameters with a null buffer, then with a buffer of the reported size, and passes what came back to the primitive setter
- **THEN** it receives the parameter count, then the parameters as authored, and the setter accepts them

#### Scenario: A buffer that is too small reports what it needed
- **WHEN** the query is given a buffer shorter than the primitive's arity
- **THEN** it reports a buffer-too-small failure with the needed count and writes nothing into the buffer

#### Scenario: An out-of-line primitive counts zero
- **WHEN** the query names a placed stroke
- **THEN** it reports a count of zero rather than a refusal, and its points remain readable through the stroke reader

### Requirement: A placed node's op and blend read back for a group as well as an item
The op query SHALL report the op, the blend profile, the blend radius and the rounding — the four values the op/blend setter takes — and SHALL answer for a GROUP as well as for an item, because a group carries all four and its setter writes them. This makes it the one of the three readers with no group refusal, which is deliberate and SHALL be stated at the call.

A group created with the inline op SHALL read back the blend, radius and rounding it was required to be created with, since an inline group consults none of them.

#### Scenario: An item's operation round trips
- **WHEN** an item's op, blend, blend radius and rounding are set and then read back
- **THEN** all four match what was set, and passing them back to the setter is accepted

#### Scenario: A group answers for its op
- **WHEN** the op query names a group
- **THEN** it reports the group's op, blend, radius and rounding rather than refusing

## MODIFIED Requirements

### Requirement: Node and layer editing across the ABI
The C API SHALL expose the same editing surface as the Python bindings: node transform, primitive, colour, op/blend/rounding, move and remove; layer add, remove, reorder, visibility and transform; stroke append and trim. Edits SHALL be addressed by node or layer id and SHALL return `CLAY_ERROR_NOT_FOUND` for an id the document does not hold, leaving the document unchanged.

Unlike the Python bindings, which take partial updates, the C setters take the WHOLE value, because C has no idiomatic "leave this one alone" argument. That is only workable if the current value is readable, so the ABI SHALL carry a reader for each whole-value setter a host is expected to edit in place — read the current state, change what you want, pass all of it back. Colour is the one setter without a reader, and its absence SHALL be a recorded gap rather than an oversight.

#### Scenario: Editing from Swift
- **WHEN** a C consumer adds an item, keeps its node id, and later sets a new transform and a new blend on it
- **THEN** the document evaluates identically to the same edits made through `pyclay`

#### Scenario: Unknown id is refused
- **WHEN** an edit names a node or layer id the document does not hold
- **THEN** the call returns `CLAY_ERROR_NOT_FOUND` and the document is bit-identical to before

#### Scenario: Editing a primitive keeps the modifiers
- **WHEN** a node's primitive is replaced on an item carrying a deformer chain
- **THEN** the deformer chain, repetition and profile survive the edit

#### Scenario: A partial edit is read, changed and written back
- **WHEN** a host holding only a node id wants to change its scale and nothing else
- **THEN** it reads the transform, replaces the scale and passes the whole transform back, without having kept a copy of the position and rotation of its own
