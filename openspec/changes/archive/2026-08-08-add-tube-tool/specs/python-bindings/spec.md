# python-bindings — the Tube tool

Delta for `add-tube-tool`.

## ADDED Requirements

### Requirement: Drawing a tube from Python
The module SHALL expose resolving a path into a tube, with the point type, the start/middle/end radii, closure and an optional profile under the caller's control, and SHALL say which choices cost exactness.

#### Scenario: A script draws a tapered tube
- **WHEN** a script resolves a path with a wide start and a narrow end
- **THEN** it receives an item that tapers along its length and can be added to a layer
