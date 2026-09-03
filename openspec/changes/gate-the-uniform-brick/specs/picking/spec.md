# picking — a pick pays for its own ray, not for the document

Delta for `gate-the-uniform-brick`.

## ADDED Requirements

### Requirement: A pick marches the tape it is handed, at the step scale its own ray needs
The scene raycast SHALL accept an already compiled pickable tape and the document's cull index, so that a pick made through the C ABI marches the per-revision tape the ABI already holds rather than compiling the document it was just handed. The library entry point that takes only a document SHALL keep its signature and compile as before, and the two SHALL report the same hit — the same *t* and the same step count, bit for bit — for the same ray.

The march steps by the field times the tape's safe step scale, and that scale is ONE Lipschitz bound folded over every visible node: one steep item anywhere lowers it for every ray. When the whole tape's scale is below 1 the march MAY compile a tape culled to the ray's own clipped segment and step by that tape's scale, and SHALL do so only when that scale is strictly larger. An item whose influence bound misses the segment is dropped and its Lipschitz contribution with it; the culled field SHALL be exact wherever the march evaluates, so the hit is the same surface with the step length changed. A ray that passes through the steep item SHALL march the whole tape exactly as it did, since the scales tie and nothing is won.

The hit SHALL report the number of march samples it took, because the same hit in fewer steps IS the claim and there is no other way to see it from outside.

#### Scenario: A far steep item does not slow a ray that never nears it
- **GIVEN** a sphere, a chain of blended dabs and a twisted box two units away, whose tape's step scale is below 1
- **WHEN** a ray that misses the box is cast with and without the ray-local tape
- **THEN** both hit the same layer and item within 1e-3 in *t*, the ray-local march takes strictly fewer steps, and the tape culled to the segment has a step scale of 1

#### Scenario: The cached tape reproduces the compiled one
- **WHEN** the same ray is cast through the document entry point and through the tape entry point, with the cull index and without it
- **THEN** *t* and the step count are bit-identical across all three

#### Scenario: A ray through the steep item marches the whole tape
- **WHEN** a ray that passes through the twisted box is cast with and without the ray-local tape
- **THEN** it hits the box with *t* and the step count bit-identical, because no local tape wins

### Requirement: Attribution compiles the item, not a document
The item under a hit SHALL be named by the field of a tape holding that item alone — the item as an Add, under the layer's placement and mirror — compiled directly from the layer and the node, without constructing a document to hold it. That single-item compile SHALL be byte-identical to compiling a layer that holds only that item: the same instructions, parameters, blob, info and bounds. A group SHALL yield an empty tape, since a group is not an item a hit can name.

Given the pickable tape the hit was found on, attribution SHALL read the winning layer off that tape when exactly one layer is a candidate — visible, unghosted, an SDF layer with content — because with one candidate the document's pickable tape IS that layer's tape. With several candidates it SHALL compile each candidate layer's tape, as before: the union cannot say which layer the position is nearest. The tape given SHALL be the document's own pickable tape; the contract is stated on the entry point and not defended at runtime.

The ids a hit attributes to SHALL be unchanged by any of this.

#### Scenario: The single-item compile is the single-item layer's compile
- **WHEN** every non-group item of a corpus of documents is compiled alone, and a layer holding only that item as a childless Add is compiled with the layer compile
- **THEN** the two tapes are byte-identical, and a group compiles to an empty tape

#### Scenario: A hit names the layer and item it always did
- **GIVEN** documents with mirrored, per-axis scaled, squashed, ghosted, hidden and radial layers, groups, subtracts, paints and strokes, a single layer of overlapping dabs, one candidate layer beside a ghost, and a layer whose only tape is empty
- **WHEN** both attribution entry points are compared with the previous implementation over a lattice of points, a scatter of points and hits from eight directions
- **THEN** every layer id and item id agrees

### Requirement: The brick raycast is analytic on the cache's reconstruction
The field a brick cache answers is the trilinear interpolation of its lattice, so along a ray inside one cell it is a cubic in *t*. The brick raycast SHALL find the first crossing in a cell as a root of that cubic rather than by sampling towards it: a brick-level walk that skips uniform bricks — Inside, Outside and never-evaluated alike — whole, a cell-level walk under the Surface bricks, and per cell the first root in the cell's interval. The normal SHALL be the field's own gradient in the cell that was hit.

The hit SHALL be the crossing the sphere trace it replaced stopped at — where the field falls below `eps` scaled by the distance — so that a host that picked with the old march picks the same point, and a ray starting below that threshold SHALL report its origin, as before. The step budget SHALL NOT bound the walk: it is bounded by the surface bricks' box.

The sphere trace SHALL be kept as the reference the walk is held to, under a name that says it is not the raycast, and SHALL NOT be reachable from the C ABI.

#### Scenario: The analytic walk agrees with the sphere trace
- **GIVEN** a cache filled over a worked sculpt
- **WHEN** hundreds of rays are cast with both — from a sphere around the model aimed near it, from inside the model's box including inside Surface bricks and inside the solid, and axis-aligned along lattice and brick faces
- **THEN** every ray hits or misses the same way, every hit's *t* is within a twentieth of a voxel of the reference, and every crossing's normal is within 26° of the reference and faces the ray
