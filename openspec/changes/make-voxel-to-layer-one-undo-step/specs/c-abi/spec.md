## MODIFIED Requirements

### Requirement: A host converts a sculpt into a layer it can keep working on
The C ABI SHALL let a host convert a voxel sculpt into a layer of operands, so the sculpt can be booleaned, blended and deformed again rather than only displayed or exported.

The conversion SHALL be NON-DESTRUCTIVE: it SHALL create a new layer and SHALL leave the grid and the original layer untouched, so a host can offer "go back" by keeping what it had. The conversion is irreversible in what it discards — the procedural history — and a destructive default would cost a parametric model to one misclick.

It SHALL place ONE volume item that carries the grid's palette per sample, so every colour the sculpt holds survives the trip without one item per palette entry. Colour is authored data and a trip that drops it is unattractive whatever it does to the geometry.

It SHALL introduce no new layer kind and SHALL NOT move the document format version: the result is an ordinary volume item in an ordinary SDF layer.

The conversion SHALL be ONE undo step: with undo enabled, the call SHALL grow the undo depth by exactly one, one undo SHALL remove the layer together with its item, and one redo SHALL restore both. A host maps one user action to one undo, and a conversion that undoes in two presses leaves an empty layer standing between them.

A conversion that cannot produce anything SHALL fail without having modified the document, rather than leaving an empty layer behind.

#### Scenario: The converted sculpt is an operand
- **WHEN** a host converts a two-colour voxel sculpt into a layer
- **THEN** the layer holds one item whose volume carries both colours, and evaluating the layer reports solid inside the sculpt and each colour inside its own half

#### Scenario: The original survives the conversion
- **WHEN** a sculpt is converted
- **THEN** the grid still holds the cells it held, and the layer it lives in is unchanged

#### Scenario: An empty grid leaves no wreckage
- **WHEN** a grid holding nothing is converted
- **THEN** the call is refused and the document has gained no layer

#### Scenario: One conversion is one undo step
- **WHEN** a host with undo enabled converts a sculpt into a layer and then undoes once
- **THEN** the undo depth grew by exactly one across the conversion, the one undo leaves the document with no converted layer, and one redo brings back the layer holding its single volume item
