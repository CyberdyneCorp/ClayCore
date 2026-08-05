# c-abi — ghosted and locked layers

Delta for `add-layer-ghost-lock`.

## ADDED Requirements

### Requirement: The flags across the ABI
The C API SHALL expose reading and setting a layer's ghost and lock flags, and SHALL return a typed error for an edit naming a protected layer.

#### Scenario: Editing a protected layer is a typed error
- **WHEN** a C consumer adds an item to a ghosted layer
- **THEN** the call returns an error naming the protection and the document is unchanged
