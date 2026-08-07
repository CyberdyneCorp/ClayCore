# python-bindings — flattening a surface

Delta for `add-sdf-flatten`.

## ADDED Requirements

### Requirement: Flattening from Python
The module SHALL expose flattening a volume against a plane, with the plane, the strength, the step, the iteration count and the region under the caller's control.

The binding SHALL state that flatten BAKES, for the same reason relax does, and SHALL state that the plane is the caller's to supply — the engine has no camera and does no picking.

#### Scenario: A document is flattened into a volume
- **WHEN** a script samples a document, flattens it against a plane and adds the result to a layer
- **THEN** the document's field has a facet on that plane

#### Scenario: The parameters do what they say
- **WHEN** a script flattens the same shape with increasing iterations
- **THEN** the surface is progressively closer to the plane

#### Scenario: A degenerate plane is refused
- **WHEN** a script passes a zero-length plane normal
- **THEN** it gets an error rather than a volume shaped by an arbitrary direction

### Requirement: An imported shape can be faceted from the C ABI
The C ABI SHALL be able to flatten an item that carries a volume, beside the existing relax entry point, so an imported scan can be faceted from an app. Asking it to flatten an item carrying anything else SHALL be refused rather than ignored.

#### Scenario: A C-built volume is flattened
- **WHEN** a C caller samples a mesh into an item and flattens it against a plane
- **THEN** the item's field has a facet on that plane

#### Scenario: Flattening something that is not a volume is refused
- **WHEN** a C caller asks to flatten an item carrying an ordinary primitive
- **THEN** the call fails rather than silently doing nothing
