# scene-model — one gesture is one step, and a volume can be installed on its own

Delta for `add-sdf-sculpt-transaction`.

## ADDED Requirements

### Requirement: Undo brackets nest
Opening an undo group inside an already-open one SHALL NOT start a second step. Nested brackets SHALL collapse into the outermost one, and the step SHALL close when the outermost bracket closes.

Without this an entry point that groups its own work cannot be called from inside a caller's group without splitting one gesture into two undos — and the case is ordinary rather than exotic: consolidation brackets its own sever-and-install, and a sculpt gesture that commits a stroke and then consolidates it must be one thing for the artist to undo, in one step they can see the far side of.

An unbalanced close SHALL be ignored rather than corrupt the stack: the next command SHALL open its own step as it would have. An outermost bracket that recorded nothing SHALL still record nothing, including when it contained only empty inner brackets.

#### Scenario: An inner bracket does not open a step
- **WHEN** commands are recorded inside a bracket that itself contains a bracket
- **THEN** the stack has gained exactly one step, one undo restores the document to before the outermost bracket, and one redo restores it to after

#### Scenario: An unbalanced close is ignored
- **WHEN** a group is closed that was never opened, or closed once more than it was opened
- **THEN** the stack is intact and the next command opens its own step

#### Scenario: Empty brackets record nothing
- **WHEN** a bracket containing only another empty bracket is opened and closed
- **THEN** the stack has gained nothing

### Requirement: An already-computed volume can be installed as a layer's content
Consolidation is two things sold together — sample the layer's field into a volume, then replace the layer's edit list with that volume — and the second SHALL be reachable on its own.

A caller that already holds a volume for a layer SHALL be able to install it as that layer's single item without the layer being sampled again. The installer SHALL be the same code the collapsed form runs, so everything the collapsed form guarantees holds here: shared instance content is severed first so that baking one subtool does not collapse its duplicates; the removals and the add are ONE undo step whose inverse restores the absorbed items with their ids, parameters, colours and deformers; a protected layer is refused; the layer's own transform is left where it was authored; and the first absorbed item's colour survives onto the volume.

The installer SHALL NOT check that the volume is a plausible bake of that layer. It cannot — a volume is a volume — and the caller owns that claim.

#### Scenario: Installing a volume is what consolidating installs
- **GIVEN** two identical documents
- **WHEN** one is consolidated and the other has the volume of its own bake installed with the same parameters
- **THEN** the two documents serialize to the same bytes, and both report the layer as consolidated

#### Scenario: Installing a volume is one undoable step
- **WHEN** a volume is installed on a layer with an undo stack
- **THEN** the stack has gained one entry, undoing restores the layer's items exactly, and redoing restores the volume

#### Scenario: Installing a volume severs shared content and refuses a protected layer
- **WHEN** a volume is installed on a layer whose edit list is shared with another
- **THEN** the other layer's items are untouched, and one undo restores both the absorbed items and the sharing
- **AND WHEN** installation is attempted on a locked layer, a ghosted layer or an unknown layer
- **THEN** it is refused and the document is unchanged
