# python-bindings — relaxing a surface

Delta for `add-sdf-relax`.

## ADDED Requirements

### Requirement: Relaxing from Python
The module SHALL expose relaxing a volume, with the strength, the kernel radius, the iteration count and the region all under the caller's control.

The binding SHALL state plainly that relax BAKES: what comes back is a sampled volume, not the edit list that went in, so the items and their editable parameters are gone. That is inherent to relaxing a field rather than a limitation of the implementation, and a caller needs to know it before choosing a resolution.

#### Scenario: A document is relaxed into a volume
- **WHEN** a script samples a document, relaxes it and adds the result to a layer
- **THEN** the document's field is the smoothed shape

#### Scenario: The parameters do what they say
- **WHEN** a script relaxes the same shape at increasing strength or iterations
- **THEN** the surface is progressively smoother

### Requirement: An imported shape can be smoothed from the C ABI
An app's reachable workflow is to import a mesh and then smooth it, so the C ABI SHALL be able to relax an item that carries a volume. Asking it to relax an item that carries anything else SHALL be refused rather than ignored.

#### Scenario: A C-built volume is relaxed
- **WHEN** a C caller samples a mesh into an item and relaxes it
- **THEN** the item's field is the smoothed shape

#### Scenario: Relaxing something that is not a volume is refused
- **WHEN** a C caller asks to relax an item carrying an ordinary primitive
- **THEN** the call fails rather than silently doing nothing
