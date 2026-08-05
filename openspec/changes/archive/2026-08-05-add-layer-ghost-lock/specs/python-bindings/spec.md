# python-bindings — ghosted and locked layers

Delta for `add-layer-ghost-lock`.

## ADDED Requirements

### Requirement: The flags from Python
The module SHALL expose reading and setting a layer's ghost and lock flags, and SHALL raise on an edit to a protected layer rather than silently dropping it.

#### Scenario: Editing a locked layer raises
- **WHEN** a script adds an item to a locked layer
- **THEN** an error is raised and the layer is unchanged
