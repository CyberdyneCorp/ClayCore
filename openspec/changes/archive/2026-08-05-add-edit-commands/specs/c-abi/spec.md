# c-abi — editing an existing document

Delta for `add-edit-commands`.

## ADDED Requirements

### Requirement: Node and layer editing across the ABI
The C API SHALL expose the same editing surface as the Python bindings: node transform, primitive, colour, op/blend/rounding, move and remove; layer add, remove, reorder, visibility and transform; stroke append and trim. Edits SHALL be addressed by node or layer id and SHALL return `CLAY_ERROR_NOT_FOUND` for an id the document does not hold, leaving the document unchanged.

#### Scenario: Editing from Swift
- **WHEN** a C consumer adds an item, keeps its node id, and later sets a new transform and a new blend on it
- **THEN** the document evaluates identically to the same edits made through `pyclay`

#### Scenario: Unknown id is refused
- **WHEN** an edit names a node or layer id the document does not hold
- **THEN** the call returns `CLAY_ERROR_NOT_FOUND` and the document is bit-identical to before

#### Scenario: Editing a primitive keeps the modifiers
- **WHEN** a node's primitive is replaced on an item carrying a deformer chain
- **THEN** the deformer chain, repetition and profile survive the edit
