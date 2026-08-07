# c-abi — document grab

Delta for `add-document-grab`.

## ADDED Requirements

### Requirement: Moving a surface across the C ABI
The ABI SHALL expose applying a world drag to a document, and previewing which items it would reach without touching the document, so a host application writing a Move brush does not reimplement the world-to-local mapping the engine already owns.

Setting an arbitrary deformer chain is deliberately NOT exposed. Nothing across this boundary needs it: a host builds deformers on an item through the item builder before adding it, and the one edit that changes a chain on an existing item is this drag. An entry point added for symmetry alone would be a second way to say something no caller is asking for.

Parameters SHALL cross as a versioned descriptor struct, following the convention every other multi-parameter entry point here uses.

#### Scenario: A host drags a surface
- **WHEN** a host resolves and applies a world drag to a document
- **THEN** the surface moves as one and the edit is on the document's undo stack

#### Scenario: A drag that reaches nothing is not an error
- **WHEN** a drag is applied whose radius reaches no item
- **THEN** it reports success having edited nothing, rather than failing

#### Scenario: A host previews before committing
- **WHEN** a host asks which items a drag would reach
- **THEN** it receives them without the document being modified, following the size-query convention
