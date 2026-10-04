## ADDED Requirements

### Requirement: A host can set and read an item's own mirror axes
The C ABI SHALL expose an item's own mirror axes on the builder (`clay_item_set_mirror_axes`, `clay_item_mirror_axes`) and on a placed item (`clay_layer_set_node_mirror`, `clay_layer_node_mirror`), with `CLAY_MIRROR_AXES_INHERIT` meaning "follow the layer". Axes outside `CLAY_MIRROR_X|Y|Z` that are not the inherit value SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT`.

`clay_layer_set_node_mirror` SHALL set the participation flag and the own axes as one undoable edit. It SHALL refuse a group with `CLAY_ERROR_INVALID_ARGUMENT`, since evaluation never reads a group's mirror, and SHALL answer `CLAY_ERROR_NOT_FOUND` for a node that is not there. `clay_layer_node_mirror` SHALL report the participation flag, the own axes, and the axes the item is actually reflected through on its layer.

#### Scenario: A builder starts inheriting
- **WHEN** a new item builder's own axes are read
- **THEN** they are `CLAY_MIRROR_AXES_INHERIT`

#### Scenario: A placed item is corrected in place
- **WHEN** a host sets a placed item's own axes and participation, then undoes
- **THEN** the reader reports the new values and the effective axes follow them, and after the undo it reports the old ones

#### Scenario: Invalid axes and groups are refused
- **WHEN** axes of 8 are passed, or a group is named
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and nothing changes

### Requirement: Writing below minor 20 refuses an item's own mirror axes
`clay_document_writable_at_minor`, `clay_document_save_at_minor` and `clay_document_save_memory_at_minor` SHALL refuse any minor below 20 for a document holding an item with its own mirror axes. They SHALL return `CLAY_ERROR_UNSUPPORTED`, name the first such layer, and give a message that names the own axes as the reason. Writing below 20 would give the item the layer's copies instead of its own.

#### Scenario: The blocking layer is named
- **WHEN** a document holding an item with its own axes X is asked whether it can be written at minor 19
- **THEN** the answer is `CLAY_ERROR_UNSUPPORTED`, the item's layer is named, and a save at 19 writes nothing
