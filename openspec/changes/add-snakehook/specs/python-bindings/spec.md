# python-bindings — snakehook

Delta for `add-snakehook`.

## ADDED Requirements

### Requirement: Pulling a tendril from Python
The module SHALL expose resolving a drag into a tendril, with the anchor, the normal, the path, the base and tip radii and the taper under the caller's control.

The binding SHALL state that this ADDS material rather than moving it: ZBrush's snakehook pulls existing surface, so the body dimples slightly where the tendril came from, and this does not.

#### Scenario: A script pulls a horn
- **WHEN** a script resolves a drag from a sphere's surface and adds the result to the layer
- **THEN** the document's field is the sphere with a tendril attached

#### Scenario: The taper is under control
- **WHEN** a script resolves the same drag with different tip radii
- **THEN** the tendril ends thicker or thinner accordingly
