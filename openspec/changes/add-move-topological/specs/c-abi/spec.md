# c-abi — Move Topological

Delta for `add-move-topological`.

## ADDED Requirements

### Requirement: A topological move across the C ABI
The ABI SHALL expose applying a topological move to an item carrying a volume, with its parameters in a versioned descriptor struct as every other multi-parameter entry point here uses. An item that carries no volume, a radius that is not positive, or an unknown easing curve SHALL be refused rather than ignored.

#### Scenario: A host moves one part and not its neighbour
- **WHEN** a host applies a topological move to a volume holding two parts close in space and joined only through a distant path
- **THEN** only the part connected to the anchor along the material moves

#### Scenario: It refuses what it cannot do
- **WHEN** the call names an item with no volume, or a radius that is not positive
- **THEN** it is refused rather than silently doing nothing
