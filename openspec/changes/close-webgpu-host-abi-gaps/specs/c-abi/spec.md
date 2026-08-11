# c-abi — what a GPU host needs from the boundary

Delta for `close-webgpu-host-abi-gaps`.

## MODIFIED Requirements

### Requirement: The brick cache across the C ABI
The ABI SHALL expose the brick cache as an opaque handle the caller creates from a versioned configuration descriptor and destroys, never bound to a document, alongside the three calls that make it usable: dense-grid evaluation with an optional cull region, and the influence bound of a node and of a layer.

There SHALL be exactly one refill path — mark dirty, drain requests, evaluate, submit — with the drain taking a capacity and reporting a count and a remainder rather than accepting a NULL buffer as a size query, and with the outcome of a submission (accepted, stale, over budget) crossing as an out-parameter with a success return, on the same footing as "nothing to undo".

The refill path SHALL carry colour when the cache was configured for it: the batched evaluation call SHALL be able to produce a colour lattice beside the distances, and submission SHALL accept it, so that colour reaches the cache by the same route distance does and there is no second path to keep consistent.

The handle SHALL take no lock and start no thread; the header SHALL state that calls on one handle are the host's to serialize and that the batched evaluation call, which takes no handle, is free-threaded against one const document.

A dirty region SHALL be validated in 64-bit before the engine converts it: a non-finite or empty region, a brick coordinate outside `int32`, and a span above the batch ceiling SHALL each be refused with the cache left unchanged. Dirtying everything the cache tracks SHALL be spelled as the absence of a region, never as a region carrying an infinity.

Brick readback SHALL write a consumer's own buffer at a fixed stride in the engine's stored bits, and SHALL accept an optional apron so that the stride is a padded, directly filterable tile when the consumer asks for one. The buffer length SHALL continue to be required exactly rather than inferred, against whichever stride the call was asked for.

Meshing the cache SHALL accept an optional set of brick keys and an optional per-key range report, so a consumer can re-mesh and re-upload what a drain reported dirty. Passing no set SHALL mean every surface brick, which is the behaviour a caller has today.

Brick raycasting SHALL have a batched form matching the document-level batched raycast in ray layout and in optional outputs.

#### Scenario: A host refills from the header alone
- **WHEN** a consumer with only `clay.h` marks a layer's influence bound dirty, drains the requests in fixed-size chunks, evaluates them and submits the values
- **THEN** every brick is accepted, the pending count reaches zero, and the surface bricks read back at a fixed stride in the engine's own fp16 bits

#### Scenario: A count is never inferred
- **WHEN** a value buffer is passed whose length is not exactly the request count times the brick's sample count
- **THEN** the call is refused rather than reading or writing what the caller did not allocate

#### Scenario: A request carries everything its evaluation needs
- **WHEN** a request is drained and evaluated
- **THEN** the lattice AND the band come with it, so the evaluator culls against the brick dilated by the band without consulting the cache, and there is no value a caller can supply wrongly

#### Scenario: The bound to dirty is not the bound to frame on
- **WHEN** a consumer asks for a node's influence bound
- **THEN** it receives a box no tighter than the layer bounds query reports, and an explicit flag for the items whose influence is unbounded

#### Scenario: A host uploads a colour atlas without meshing
- **WHEN** a consumer configures a cache for colour, refills it, and reads the surface bricks back with colour and a one-voxel apron
- **THEN** it holds directly uploadable, directly filterable distance and colour tiles for the whole narrow band, having compiled no kernel of its own

#### Scenario: A dirty drain feeds the mesher
- **WHEN** the keys a drain reported are handed to the cache mesher as its key set
- **THEN** only those bricks are marched, and the per-key ranges name where each landed in the output

## ADDED Requirements

### Requirement: A mesh can be copied into a caller's own vertex layout
The ABI SHALL let a consumer copy a mesh's vertices into memory it owns, in an interleaved layout it describes, and its indices likewise — so that geometry reaches a mapped GPU buffer in one pass rather than through an interleave into a staging buffer followed by a copy.

The layout SHALL be a versioned descriptor naming a stride and a byte offset per attribute, with an attribute omitted by naming no offset for it. It SHALL describe placement only: attributes are copied in the float form the mesh holds, and format conversion is not part of this descriptor.

The destination length SHALL be required exactly and checked, never inferred, consistent with every other call that writes into a consumer's buffer. Requesting an attribute the mesh does not carry SHALL be refused rather than filled with a default, because a silently black or silently flat model is harder to diagnose than a returned error.

The mesh SHALL remain the engine's, produced and freed as it is today; this requirement adds a copy-out and SHALL NOT make meshing write into caller memory, since a mesher cannot report its vertex count before it has run.

#### Scenario: One pass into a mapped buffer
- **WHEN** a consumer describes a position/normal/colour interleaved layout and copies a mesh into a buffer sized from the mesh's vertex count
- **THEN** the buffer holds each vertex's attributes at the named offsets and stride, and the data matches the deinterleaved accessors element for element

#### Scenario: An absent attribute is refused, not invented
- **WHEN** a layout names a colour offset for a mesh that was meshed without colours
- **THEN** the call is refused and nothing is written

#### Scenario: A short destination is refused
- **WHEN** the destination is smaller than the stride times the vertex count
- **THEN** the call is refused rather than writing what the caller did not allocate

### Requirement: Every primitive is reachable on a host preview path
A consumer drawing this library's field on its own GPU SHALL be able to reproduce EVERY primitive the document can contain, including sampled volumes, without enumerating primitive kinds in its own code.

A host that implements a subset of the dialect draws a subset of the document, and the subset that goes missing is not arbitrary: sampled volumes are what every regional verb produces, so a preview lacking them shows nothing for a whole class of brush until a bake lands. Neither published path SHALL require a consumer to name primitives — one evaluates the compiled tape, whose out-of-line payload carries a volume's samples, and the other reads bricks filled by evaluating that same tape.

#### Scenario: A regional verb is visible before the bake
- **WHEN** a document containing a sampled volume is drawn through either the exported tape or the brick payloads
- **THEN** the volume contributes its surface, and the field a consumer evaluates agrees with the library's own at the same points

#### Scenario: A volume is found wherever it sits in the payload
- **WHEN** a sampled volume follows another item that carries out-of-line data, so it does not begin at the start of the payload
- **THEN** it is still evaluated correctly on both paths
