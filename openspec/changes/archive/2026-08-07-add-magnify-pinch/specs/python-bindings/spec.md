# python-bindings — magnify and pinch

Delta for `add-magnify-pinch`.

## ADDED Requirements

### Requirement: Magnify and pinch from Python
The module SHALL expose the radial scale deformer on items, alongside the other deformers, and the voxel magnify verb alongside the other sculpt verbs.

The binding SHALL state that one signed strength covers both directions, so a caller does not go looking for a separate pinch.

#### Scenario: A script swells a feature
- **WHEN** a script adds the deformer to an item with a positive strength
- **THEN** the document's field shows the surface swelled about that centre

#### Scenario: A script creases an edge
- **WHEN** the same call is made with a negative strength
- **THEN** the surface gathers toward the centre instead
