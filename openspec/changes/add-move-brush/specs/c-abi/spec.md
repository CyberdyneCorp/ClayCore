# c-abi — the Move brush

Delta for `add-move-brush`.

## ADDED Requirements

### Requirement: A host can move a surface through the C ABI
The ABI SHALL expose the Move brush as one call taking a world centre, radius and displacement and applying the resolved warps to a layer, and SHALL report how many items were affected so a host can tell "the drag missed everything" from "the drag did nothing visible".

It SHALL also expose replacing a node's deformer chain, since that is the mutation the Move is built on and a host has no other way to reach it: `clay_item_add_deformer` acts on a builder, not on a node already in a document.

With undo enabled the whole move SHALL be ONE step however many items it touched, using the existing grouping — a drag is one gesture, and undoing it item by item would be an artifact of the implementation showing through.

#### Scenario: A host drags a blended form
- **WHEN** a host resolves a drag over a layer built from several blended items
- **THEN** the surface moves as one, and the call reports how many items took a warp

#### Scenario: One gesture, one undo step
- **WHEN** a move touching several items is undone
- **THEN** the whole drag reverts in a single step

#### Scenario: A drag that reaches nothing
- **WHEN** a drag is placed far from every item
- **THEN** the call succeeds, reports zero items affected, and changes nothing
