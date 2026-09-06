# python-bindings — mesh layers from Python

Delta for `add-mesh-layers`.

## ADDED Requirements

### Requirement: Mesh layers from Python
The module SHALL expose attaching a loaded mesh to a document as a layer, listing and fetching mesh layers, the mesh's bounds, and the combined export that appends every visible mesh layer to the meshed field. A fetched mesh SHALL be borrowed from the document and SHALL expose its buffers through the module's existing numpy exchange rather than a copy per read.

The surface SHALL land in the same change as the C one. The binding parity gate walks `pyclay` and demands a C counterpart or a recorded exemption, and it is one-way, so a Python-only surface would be caught while a C-only surface would not.

#### Scenario: A script imports a model and keeps it
- **WHEN** a script loads a mesh, attaches it to a document, saves and reloads
- **THEN** the mesh layer is back with the same arrays

#### Scenario: The buffers are numpy views
- **WHEN** a script reads a document mesh layer's positions and indices
- **THEN** it gets numpy arrays over the engine's own memory, with no per-read copy

#### Scenario: Parity holds both ways
- **WHEN** the same sequence — attach, transform, export — is run through `pyclay` and through the C ABI
- **THEN** the exported meshes are identical, and the parity gate reports no unexempted Python-only entry point
