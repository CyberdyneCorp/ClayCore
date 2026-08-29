# file-io — persisting a layer stack

Delta for `add-mesh-sculpt-layers`.

## ADDED Requirements

### Requirement: A sculpt layer stack is serialized with its surface
A sculpt layer stack SHALL serialize inside the multiresolution surface's versioned format — per layer: identity, name, kind, visibility, lock, strength, per-level detail blocks and any mask — and SHALL NOT be written into the flat mesh stream, whose readers expect interchange arrays.

Layer identities SHALL survive a save and a load, because a host, a journal and an undo step all hold them.

The layer kind SHALL be versioned from the first release, so that a later procedural layer does not require a format break.

The format SHALL remain BACKWARD-OPEN: a reader predating the stack SHALL open the document with the surface it can read rather than failing, and SHALL NOT silently present a partially composited surface as the whole one.

#### Scenario: A stack round-trips
- **WHEN** a surface carrying several layers with different strengths and visibilities is saved and reloaded
- **THEN** every layer's identity, properties and detail are restored, and the evaluated surface is identical

#### Scenario: An older reader does not misrepresent the surface
- **WHEN** a reader predating the stack opens a document containing one
- **THEN** it either reports that it cannot present the surface or presents it flattened, and does not present a partial composite as complete
