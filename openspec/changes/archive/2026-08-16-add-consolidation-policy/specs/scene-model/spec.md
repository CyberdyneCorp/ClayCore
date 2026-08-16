# scene-model — a consolidation policy

Delta for `add-consolidation-policy`.

## ADDED Requirements

### Requirement: Consolidation is one undoable command
Collapsing a region of an edit list into a volume SHALL be a single command, refused on a protected layer, whose inverse restores the items it absorbed — which means the undo record carries them rather than only the resulting volume.

The scope of a consolidation SHALL be a LAYER. An arbitrary run of siblings has
no well-defined field of its own: an edit list is ordered and its operators are
relative, so a Subtract in the middle of a list means nothing without what
precedes it. A layer does have one, because layers combine by hard union at the
document level — so baking a layer is exact with respect to the whole
document's result.

Consolidation SHALL sample the layer in its OWN frame, so the layer's transform
still applies to the result and consolidating moves nothing.

Items that are hidden SHALL NOT be absorbed. They contribute nothing to the
field, so absorbing them would spend their parameters on nothing.

#### Scenario: Consolidation undoes to the parametric form
- **WHEN** a region is consolidated and the command undone
- **THEN** the original items are present and editable by their parameters again

#### Scenario: One step, however many items
- **WHEN** a layer holding several items is consolidated
- **THEN** the undo stack grows by exactly one step

#### Scenario: A protected layer is refused before it is resampled
- **WHEN** consolidation names a ghosted or locked layer
- **THEN** it is refused and the document is unchanged, without the bake being performed

### Requirement: What a consolidated region still promises
The module SHALL state what survives consolidation and what does not: the surface within the baked resolution survives, and the parameters of the absorbed items do not — nor do their individual colours, since a volume carries one.

A host SHALL be able to tell which regions of a document are consolidated, so it can stop offering parameter edits there rather than failing them.

That answer SHALL come from the CONTENT rather than from a stored provenance
flag: a region is consolidated when its edit list is a single item carrying
samples. The promise a host has to make is about what the region IS — samples
at a fixed resolution, with no parameters to offer — and a mesh imported as a
volume is exactly as unparametric as a bake, so a flag distinguishing them
would split two cases a host must treat alike. It would also have to be
serialised to survive a save, which this change does not need.

Consolidation SHALL be one-way. What was absorbed is in the undo record, which
is where going back belongs; re-expansion would have to invent parameters for a
shape that no longer has any.

#### Scenario: A host can see what is baked
- **WHEN** a host asks about a consolidated region
- **THEN** it is told the region is baked and at what resolution

#### Scenario: A layer with a volume among other items is not consolidated
- **WHEN** a host asks about a layer holding a volume alongside parametric items
- **THEN** it is told the layer is not consolidated, because those items still have parameters to offer

### Requirement: A document that never consolidates is unchanged
Reporting a chain's degradation, quoting what consolidating would cost, and asking whether a region is consolidated SHALL all be reads: none of them SHALL change what a document serialises to or what it evaluates to.

#### Scenario: Asking costs nothing
- **WHEN** a host reports, quotes and inspects without consolidating
- **THEN** the document serialises to identical bytes and compiles to an identical tape
