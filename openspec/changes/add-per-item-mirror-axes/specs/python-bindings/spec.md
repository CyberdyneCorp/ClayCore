## ADDED Requirements

### Requirement: An item's own mirror axes are reachable from Python
pyclay SHALL let a caller give an item its own mirror axes when adding it (`Layer.add(..., mirror_axes=)`), set a placed item's participation and own axes as one undoable edit (`Layer.set_node_mirror`), and read them back (`Layer.node_mirror`). Axes are spelled as the letters of the axes they reflect across, `""` for none, and `None` for "follow the layer".

#### Scenario: A placed item's axes are one undo step
- **WHEN** an item added with `mirror_axes="x"` is set to `axes="y"` and the document is undone
- **THEN** the item reads `"x"` again and evaluates with its X twin
