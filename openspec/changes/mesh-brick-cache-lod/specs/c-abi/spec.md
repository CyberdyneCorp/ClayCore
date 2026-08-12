# c-abi — mesh a level of the brick cache

Delta for `mesh-brick-cache-lod` (#93).

## ADDED Requirements

### Requirement: A host can mesh a level of the brick cache
The C API SHALL let a host mesh a LEVEL of the brick cache, not only its full-resolution bricks. The level SHALL be named the way the readback names it — 0 for the evaluated bricks, 1 for their mips — so that a host that can build a mip and read one can also mesh one without reimplementing the mesher over the stored samples.

The addition SHALL be purely additive: the existing meshing entry point SHALL keep its signature and its behaviour, and SHALL be the new one at level 0. Both SHALL share one implementation so the two cannot drift.

At level 1 the key list SHALL name COARSE keys — the 2x2x2 block keys the mip build and the current-level query already take — and a null key list SHALL still mean every brick the level stores. Everything else about the call SHALL be unchanged by the level: the marching, the seam welding, the per-key ranges and the straddler attribution.

A level above 1 SHALL be rejected rather than clamped, for the reason the readback rejects it: there is one mip level, and silently meshing a finer one would put geometry at the wrong size on screen.

#### Scenario: A built mip meshes, and meshes coarser
- **WHEN** a host fills the cache, builds the mips covering its surface bricks, and meshes at level 1
- **THEN** it receives real geometry whose triangle count is a small multiple lower than the same surface at level 0, and whose bounds agree with the level-0 mesh's to within one coarse cell

#### Scenario: The older entry point is unchanged
- **WHEN** the same whole-cache and key-subset requests are made through the existing entry point and through the new one at level 0, with and without a document
- **THEN** the meshes are identical byte for byte — positions, normals, colours, uvs and indices — and so are the per-key ranges

#### Scenario: A level above the one that exists is refused
- **WHEN** a host asks for a level above 1, or a negative one
- **THEN** the call is refused as an invalid argument and no mesh is produced

### Requirement: An unbuilt level is reported, never answered with an empty mesh
An empty mesh already means "this cache holds no surface bricks", which is an ordinary state of a session. A level that has not been built SHALL therefore be a typed not-found rather than an empty mesh, so that a host can tell a missing mip — whose remedy is to build it — from a missing surface, whose remedy is to sculpt.

A named coarse key with no valid mip SHALL be refused, unlike level 0 where a key that stores no lattice is an ordinary uniform brick and contributes nothing: at level 1 there is no uniform mip, so an absent one means "not yet". A whole-level request SHALL be refused when the cache holds surface bricks and not one mip.

A cache holding nothing at all SHALL still mesh EMPTY at every valid level, because there is nothing there to be mistaken about.

#### Scenario: Meshing a level nobody built
- **WHEN** a host fills the cache, builds no mip, and meshes at level 1
- **THEN** the call is refused as not found, and the same request answers once the mip is built

#### Scenario: The refusal is per key
- **WHEN** one coarse key's mip is built and a different coarse key is named
- **THEN** the request naming the built key succeeds and the request naming the unbuilt one is refused as not found

#### Scenario: An empty cache is empty, not unbuilt
- **WHEN** a cache that was never marked or filled is meshed at level 0 and at level 1
- **THEN** both succeed and return a mesh with no vertices and no indices

### Requirement: Field attributes stay at level 0
Colours and gradient normals SHALL be refused at level 1 rather than approximated. They are evaluated through per-brick culled tapes whose agreement with the whole document's rests on a vertex sitting on the FIELD's surface, well inside the band; a coarse vertex sits on the mip's surface, which can be most of a coarse cell away, where the two tapes are only both out of band rather than equal. The mip also carries no colour lattice of its own, which the brick readback already reports rather than averaging.

Face normals are computed from the triangles, need no field, and SHALL work at every level — otherwise "refused" would silently mean "no normals at level 1".

#### Scenario: The refusal is about the level, not the parameters
- **WHEN** the same gradient-normal and colour requests are made with a document at level 0 and at level 1
- **THEN** level 0 succeeds and carries the attributes, and level 1 is refused as an invalid argument

#### Scenario: Face normals answer at every level
- **WHEN** a host meshes level 1 asking for face normals and passing no document
- **THEN** the mesh carries normals
