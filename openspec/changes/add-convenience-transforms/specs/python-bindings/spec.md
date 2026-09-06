# python-bindings

## ADDED Requirements

### Requirement: The convenience placements are reachable from Python

`pyclay` SHALL expose the three convenience placements on a layer — snapping to
a ground height, centring on the world origin, and returning the placement's
translation to the origin — with the same reach and the same refusals the C ABI
gives them.

Each SHALL be spelled as a member of the layer class under the name the C ABI
uses, so that the binding-parity gate satisfies it by the existing
`Layer` -> `clay_layer_` prefix rule, WITHOUT an alias-table entry and WITHOUT
an exemption. Needing either would mean the two names had diverged, which is
what the gate exists to catch.

Where the C ABI refuses — an unknown layer, a protected layer, a layer holding
no material, a layer carrying a radial mode, a ground height that is not finite
— the Python form SHALL raise rather than return a status, following the
surrounding bindings, and the document SHALL be unchanged.

Undo SHALL cover them as it covers every other reachable edit: one call is one
undoable step.

#### Scenario: Parity holds with no alias and no exemption
- **WHEN** the binding-parity gate runs against the built module and the C header
- **THEN** each of the three has a C counterpart found by the prefix rule, and neither an alias entry nor an exemption is recorded for it

#### Scenario: A refusal raises
- **WHEN** a layer holding no material is snapped from Python
- **THEN** the call raises and the document is unchanged

#### Scenario: One call is one undo step
- **GIVEN** a document with undo enabled
- **WHEN** a layer is centred from Python and the document is undone once
- **THEN** the layer carries the placement it had before, rotation and both scales included
