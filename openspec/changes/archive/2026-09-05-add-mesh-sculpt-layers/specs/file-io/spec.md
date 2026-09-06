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

### Requirement: A layer stack chunk is refused on its own terms, before it is reserved from
The stack chunk SHALL apply the same ceilings the surface stream around it applies to the same two numbers — a level count and a per-level vertex count. Both are numbers the layer decoder RESERVES FROM, and the surface's cross-check of a decoded stack against the hierarchy it rebuilt runs only after the layer decoder has returned. Where no such cross-check exists at all — a journal's structural undo record carries a whole stack snapshot and is not required to name the surface it was taken against — the decoder's own ceilings are the only refusal there is.

A stack's per-level invalidation index SHALL NOT be reserved from a declared level size. It is a cache read only while the level is partially stale, and a freshly decoded stack is wholly stale, so it SHALL be sized where it is first consulted.

A layer's per-level fields SHALL describe the stack's levels and SHALL share the stack's blocking. Block `b` naming the same vertices in a layer's coefficients, in its mask and in the level's composed field is what makes a strength change cost the layer's coverage rather than the surface, and the invalidation path hands a field's block numbers to the stack's index without translating them — so a stream pairing two blockings SHALL be refused rather than silently unsharing that index.

An unknown layer kind SHALL be refused rather than skipped. A strength outside `[0,1]`, including one that is not a number, SHALL be refused. Two layers answering to one identity, an identity at or above the serialized counter that mints the next one, and an active identity the stream does not carry SHALL each be refused.

#### Scenario: A hostile chunk costs its bytes rather than what it declares
- **WHEN** a stack chunk of under a hundred bytes declares the deepest hierarchy this build accepts, at the finest blocking the format allows, carrying no layers
- **THEN** it decodes, and the memory it reserves is proportional to the chunk rather than to the levels it names

#### Scenario: A journal snapshot naming an impossible hierarchy changes nothing
- **WHEN** a structural undo record whose stack snapshot declares a level larger than any level can be, or more levels than this build reconstructs, is replayed
- **THEN** the replay refuses, and the stack it was applied to is unchanged

#### Scenario: A stream pairing two blockings is refused
- **WHEN** a stack declaring one block size carries a layer whose coefficients or mask declare another
- **THEN** the stream is refused, rather than loading a stack whose invalidation would mark blocks the level does not have
