# python-bindings — importing a mesh as a field

Delta for `add-mesh-to-field-import`.

## ADDED Requirements

### Requirement: Sampling a mesh from Python
The module SHALL expose building a volume from a loaded mesh at a stated resolution, so that a script can import a model and immediately place it in a document.

#### Scenario: A loaded mesh becomes an item
- **WHEN** a script loads a mesh, samples it into a volume and adds it to a layer
- **THEN** the document's field describes the mesh's shape

#### Scenario: An empty mesh is refused
- **WHEN** a script samples a mesh with no triangles
- **THEN** it gets an error rather than an empty volume

### Requirement: The sign can be inspected, not merely trusted
The import rests on the claim that the generalized winding number behaves — that a hole does not flip a half-space, and that summarizing distant geometry does not move a point across the surface. The module SHALL expose the underlying queries so a script can check that claim rather than take it on trust.

The acceleration structure SHALL be built where the caller can see it rather than per query, because building it is the expensive part.

#### Scenario: The winding number can be plotted across a hole
- **WHEN** a script queries the winding number along a line passing out through an opening in a mesh
- **THEN** it gets a continuous curve passing through a half rather than a step

#### Scenario: The approximation can be checked against the exact sum
- **WHEN** a script queries the winding number with summarization disabled and with it enabled
- **THEN** it can compare them, and no point changes which side of the surface it is on

#### Scenario: A mesh loads from a file
- **WHEN** a script loads a mesh by path
- **THEN** it gets a mesh it can sample, and an unsupported extension is reported rather than guessed at
