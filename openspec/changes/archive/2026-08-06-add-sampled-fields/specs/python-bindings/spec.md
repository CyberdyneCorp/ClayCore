# python-bindings — sampled fields

Delta for `add-sampled-fields`.

## ADDED Requirements

### Requirement: Sampling a field from Python
The module SHALL expose building a volume by sampling an existing document over a region at a stated resolution, and placing the result as an item.

#### Scenario: Baking a document into a volume
- **WHEN** a script samples a document into a volume and evaluates both
- **THEN** the fields agree near the surface within the sampling tolerance

#### Scenario: The sampling can be inspected
- **WHEN** a script asks a volume for its cell size, band, brick count, size and bounds
- **THEN** it gets them, so the cost of a chosen resolution is visible before the volume is used

#### Scenario: The two halves of the bound can be told apart
- **WHEN** a script asks whether a point lands where the volume kept samples
- **THEN** it is told, so a test can hold the interpolated region and the bounded region to their different guarantees

### Requirement: The C ABI does not yet build one
A volume is only reachable by sampling something, and nothing in the C ABI can supply the samples until mesh import lands. Constructing one through the C ABI SHALL be refused rather than returning an item that could only ever be empty. The enumerator SHALL remain declared, because the value appears in saved documents, and documents containing one SHALL still load, evaluate and mesh.

#### Scenario: Constructing a volume through the C ABI is refused
- **WHEN** a C caller asks for an item of the volume primitive type
- **THEN** the call fails with an invalid-argument error rather than returning an empty item
