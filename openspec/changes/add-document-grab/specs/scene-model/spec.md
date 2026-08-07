# scene-model — setting an item's deformers

Delta for `add-document-grab`.

## ADDED Requirements

### Requirement: An item's deformer chain is editable through the command vocabulary
The vocabulary SHALL carry a command that replaces an existing item's deformer chain, whose inverse is the chain it replaced. Without it a deformer can only reach a document inside a whole node at creation, and changing one means removing and re-adding the node — which is neither cheap enough for a live drag nor honest about what changed.

The whole chain is replaced rather than edited granularly, for the same reason `SetStrokePointsCmd` replaces a whole point list: a chain is a handful of records, so a whole-list replace costs less than the bookkeeping granular commands would need, and its inverse is the previous list — exact by construction rather than by careful arithmetic.

#### Scenario: A deformer is added to an existing item
- **WHEN** the command sets a chain on an item that had none
- **THEN** the item evaluates as deformed, and the command's inverse restores the undeformed field

#### Scenario: The inverse is the previous chain
- **WHEN** the command replaces a chain and its inverse is then applied
- **THEN** the document serializes bit-identically to its state before the edit

#### Scenario: It refuses what every edit refuses
- **WHEN** the command targets a protected layer or a node that does not exist
- **THEN** it is refused rather than partially applied

#### Scenario: It round-trips through the file format
- **WHEN** a document whose history contains the command is saved and reloaded
- **THEN** the reloaded document evaluates identically and re-serializes to identical bytes
