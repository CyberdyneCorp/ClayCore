# c-abi — a host can see and set a volume's colour

Delta for `volume-color-channel`.

## ADDED Requirements

### Requirement: A host converts a coloured sculpt in one call
Converting a voxel sculpt into the document SHALL be able to produce ONE volume carrying the palette, rather than one volume per palette entry. The per-entry conversion exists because a field had nowhere to store a palette; once it does, a forty-entry sculpt SHALL NOT become forty items.

`clay_voxel_to_layer` SHALL keep its signature and produce a single item. This changes what a host counts after converting, and SHALL be stated in the header rather than discovered: a caller that counted one node per palette entry will now count one.

Converting a SINGLE palette entry SHALL remain available, because it is how a caller assembles a sculpt by hand and how a host takes one part of a sculpt as its own operand.

A host SHALL be able to ask whether a volume item carries colour, so it can tell a converted sculpt from a bake that predates this and choose what to show.

#### Scenario: A coloured sculpt converts to one item
- **WHEN** a host converts a voxel sculpt carrying several palette entries
- **THEN** the layer holds one volume item, evaluating it reports each entry's colour in that entry's region, and the item reports that it carries colour

#### Scenario: One entry still converts alone
- **WHEN** a host converts a single palette index
- **THEN** it receives an item solid only where that entry's cells are, as before
