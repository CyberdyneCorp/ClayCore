# python-bindings — Move Topological

Delta for `add-move-topological`.

## ADDED Requirements

### Requirement: A topological move from Python
The module SHALL expose sampling a document through a topological move, with the anchor, geodesic radius, displacement and easing under the caller's control, and SHALL say how it differs from the Euclidean move so a caller knows which one it wants.

#### Scenario: A script moves one finger and not its neighbour
- **WHEN** a script applies a topological move to one of two adjacent parts
- **THEN** only the part connected to the anchor along the material moves
