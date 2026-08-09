# scene-model — a consolidation policy

Delta for `add-consolidation-policy`.

## ADDED Requirements

### Requirement: Consolidation is one undoable command
Collapsing a region of an edit list into a volume SHALL be a single command, refused on a protected layer, whose inverse restores the items it absorbed — which means the undo record carries them rather than only the resulting volume.

#### Scenario: Consolidation undoes to the parametric form
- **WHEN** a region is consolidated and the command undone
- **THEN** the original items are present and editable by their parameters again

### Requirement: What a consolidated region still promises
The module SHALL state what survives consolidation and what does not: the surface within the baked resolution survives, and the parameters of the absorbed items do not.

A host SHALL be able to tell which regions of a document are consolidated, so it can stop offering parameter edits there rather than failing them.

#### Scenario: A host can see what is baked
- **WHEN** a host asks about a consolidated region
- **THEN** it is told the region is baked and at what resolution
