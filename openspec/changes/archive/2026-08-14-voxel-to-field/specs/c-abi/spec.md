# c-abi — the sculpt comes back

Delta for `voxel-to-field` (#90).

## ADDED Requirements

### Requirement: A host converts a sculpt into a layer it can keep working on
The C ABI SHALL let a host convert a voxel sculpt into a layer of operands, so the sculpt can be booleaned, blended and deformed again rather than only displayed or exported.

The conversion SHALL be NON-DESTRUCTIVE: it SHALL create a new layer and SHALL leave the grid and the original layer untouched, so a host can offer "go back" by keeping what it had. The conversion is irreversible in what it discards — the procedural history — and a destructive default would cost a parametric model to one misclick.

It SHALL place one volume item per palette entry the grid carries, each with that entry's colour, unioned without a blend. Colour is authored data and a trip that drops it is unattractive whatever it does to the geometry; a blend between the parts would round an interface that is interior to the solid they make together.

It SHALL introduce no new layer kind and SHALL NOT move the document format version: the result is ordinary volume items in an ordinary SDF layer.

A conversion that cannot produce anything SHALL fail without having modified the document, rather than leaving an empty layer behind.

#### Scenario: The converted sculpt is an operand
- **WHEN** a host converts a two-colour voxel sculpt into a layer
- **THEN** the layer holds one item per palette entry, each carrying that entry's colour, and evaluating the layer reports solid inside the sculpt

#### Scenario: The original survives the conversion
- **WHEN** a sculpt is converted
- **THEN** the grid still holds the cells it held, and the layer it lives in is unchanged

#### Scenario: An empty grid leaves no wreckage
- **WHEN** a grid holding nothing is converted
- **THEN** the call is refused and the document has gained no layer
