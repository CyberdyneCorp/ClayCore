## ADDED Requirements

### Requirement: An item may carry its own mirror axes
An item SHALL carry its own mirror axes, which are any of x/y/z, none, or "inherit". Inherit SHALL be the default. An item with its own axes SHALL be reflected through exactly those axes, whatever the layer's mirror axes are now or are later set to, and whatever its participation flag says. An item that inherits SHALL follow the layer's mirror under its participation flag exactly as before. The seam blend and the mirror planes SHALL remain the layer's, and radial participation SHALL remain governed by the participation flag alone.

Every consumer that reasons about an item's mirror copies SHALL read one definition of the axes an item is reflected through. That includes evaluation, the geometry and influence bounds, the cull pad, picking and the Move brush. Otherwise a bound or a drag could disagree with the copies evaluation emits.

A document saved before this field existed SHALL load with every item inheriting, and SHALL evaluate as it did when it was saved.

#### Scenario: Own axes override the layer
- **WHEN** a layer mirrors X and an item carries its own axes Y
- **THEN** the item has a reflection across Y and none across X

#### Scenario: Own axes of none keep an item single
- **WHEN** a layer mirrors X and an item carries its own axes of none
- **THEN** the item has no reflection

#### Scenario: Switching the layer mirror leaves an item's own axes alone
- **GIVEN** an item made under a layer mirror X that carries its own axes X
- **WHEN** the layer's mirror is turned off, and then set to Y
- **THEN** the item keeps its X twin and gains no Y reflection, while an item that inherits and is added after the switch takes Y

#### Scenario: An older document is unchanged
- **WHEN** a document written at scene minor 19, holding a participating item and an opted-out item under a layer mirror, is loaded
- **THEN** every item inherits and the field matches the saved document sample for sample

#### Scenario: The brick cache sees an item's own copy
- **WHEN** an item carries its own axes X on a layer with no mirror and a brick cache is filled over the model
- **THEN** a ray toward the twin hits it where the document tape puts it

### Requirement: A placed item's mirror is an undoable edit
Setting a placed item's participation and its own axes SHALL be one command that goes through the command vocabulary. It SHALL respect layer protection, SHALL land on the undo stack as one step, and SHALL be invertible exactly: undo restores both values the item had.

#### Scenario: Undo restores both values
- **WHEN** an item saved with the default participation is set to opt out with its own axes Y, then undone
- **THEN** the item participates and inherits again, and the field is the one before the edit; redo applies both values again
