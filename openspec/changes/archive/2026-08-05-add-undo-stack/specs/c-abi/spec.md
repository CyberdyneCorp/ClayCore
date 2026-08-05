# c-abi — undo and redo

Delta for `add-undo-stack`.

## ADDED Requirements

### Requirement: Undo across the ABI
The C API SHALL expose the same opt-in undo stack as the Python bindings: enable, undo, redo, depths and grouping. Calling undo with an empty stack SHALL report that rather than failing, so a UI can drive it without tracking state itself.

#### Scenario: Undo from Swift
- **WHEN** a C consumer enables undo, edits, and undoes
- **THEN** the document serializes bit-identically to its pre-edit state, matching what `pyclay` produces for the same sequence

#### Scenario: Empty stack is not an error
- **WHEN** undo is called on a document with nothing to undo
- **THEN** the call reports that nothing was undone without returning a failure code
