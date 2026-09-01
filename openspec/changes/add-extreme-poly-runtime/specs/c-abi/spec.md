# c-abi — the host seam at scale

Delta for `add-extreme-poly-runtime`.

## ADDED Requirements

### Requirement: A host updates what changed, with buffers it owns
The C ABI SHALL expose changed surface chunks with their revisions, and SHALL let a caller query capacity and supply its own buffers for positions, normals and indices.

The ABI SHALL NOT allocate a heap object per changed chunk per frame, and SHALL NOT require a host to copy a whole surface to observe a stamp.

A readback SHALL carry the revision it describes, so a host can discard a result that a later mutation has superseded.

An acknowledgement call SHALL let a host drain incrementally across frames without losing a change it has not yet applied.

#### Scenario: A stroke's transport follows the change
- **WHEN** a host drains changed chunks each frame during a stroke on a large surface
- **THEN** the bytes copied follow the changed chunks rather than the surface size

#### Scenario: A stale readback is identifiable
- **WHEN** a host applies a readback taken before a further mutation
- **THEN** the revisions it carries let the host detect that it is stale

### Requirement: A host sets a memory profile and asks for a trim
The C ABI SHALL accept a memory profile descriptor, SHALL expose a trim call taking a pressure level and returning what was released, and SHALL report runtime memory by the same categories the document report uses.

Descriptors SHALL follow the established `struct_size` pattern with bounded output fills.

The ABI SHALL NOT expose an entry point that releases authoritative content as part of a trim.

#### Scenario: A trim under pressure preserves the document
- **WHEN** a host sets a constrained profile and requests a critical trim
- **THEN** the call reports the released caches and the document's authoritative checksum is unchanged

### Requirement: A host's picked seed carries the numbering it was picked in
A pick that returns a seed for the surface walk SHALL also return the identity of the class space that seed was numbered in, and a stamp descriptor SHALL be able to carry it back.

A stamp given a seed whose numbering no longer matches the surface it is spent on SHALL refuse the seed and find its own starting point, rather than trusting an index that is in bounds and meaningless. The refusal SHALL be observable through a counter, so a host can tell a refused seed from one that was taken.

A caller that claims no numbering SHALL behave exactly as it did before the token existed, and an omitted field in an older caller's descriptor SHALL read as claiming nothing.

Both bindings SHALL reach this: the C ABI on the pick descriptor and the brush descriptor, and pyclay on the dict a pick returns and the keyword a stamp takes.

#### Scenario: A seed from a replaced numbering does not lose the dab
- **WHEN** a host picks a seed against one class space and stamps with it against another, carrying the token
- **THEN** the seed is refused, the counter records it, and the stamp moves the same region it would have moved with no seed at all

#### Scenario: An older host is unaffected
- **WHEN** a host compiled against the previous header sends a descriptor with no token
- **THEN** the stamp behaves exactly as it did before the field was appended

### Requirement: A host reads the peaks it tunes a profile against
The C ABI and pyclay SHALL report the runtime's high-water marks — the largest working set, the largest gathered footprint, the deepest dirty set and the most topology operations in one stamp — and SHALL let a host restart them.

They SHALL be HIGH-WATER MARKS rather than current or average values: a buffer sized to the model and reused forever allocates nothing per stamp, and only a peak that does not move between a small and a large model at the same footprint distinguishes it from a runtime whose cost follows what it touches.

A host SHALL NOT have to own or keep alive any object for this to be measured, and reading the peaks SHALL fill a descriptor the caller owns.

#### Scenario: A smaller stamp does not lower the mark
- **WHEN** a wide stamp is followed by a narrow one
- **THEN** the reported peak still describes the wide stamp, until the host resets it

### Requirement: A host drives the deferred-maintenance queue
The C ABI and pyclay SHALL expose the queue of work that is not required for correctness — the requesting of an item, the items queued, the stroke gate, and a drain — so that a host that reaches this library only through a binding can service it.

The stroke gate SHALL be enforced by the binding rather than documented: a drain attempted while a stroke is open SHALL report that there is nothing to do, and SHALL NOT run anything. Requesting an item SHALL NOT be gated, because a stamp is where an item is discovered and refusing to record one would lose the request rather than defer the work.

The drain SHALL be a take-then-complete pair rather than a callback: an item taken SHALL stay queued until the caller says it was done, so declining an item is expressible and does not drop it. Neither binding SHALL take a host function pointer for this.

An item's kind SHALL be a declared enumerator, and a value outside the declared list SHALL be refused rather than mapped onto a default.

Nothing in the queue SHALL perform the work it names; a host SHALL service an item through the ordinary entry points it already has.

#### Scenario: A drain wired to the wrong callback does nothing rather than stalling
- **WHEN** a host requests an item and attempts to drain it while a stroke is open
- **THEN** the request is recorded, the drain reports no work, and nothing is run

#### Scenario: A declined item stays queued
- **WHEN** a host takes an item and does not complete it
- **THEN** the item is still queued and the next take reports the same one

### Requirement: A host defers normals across a drag it drives itself
The C ABI and pyclay SHALL let a host defer normal recomputation for a sculptor across stamps it issues itself, read that setting back, and flush what is pending.

The committed state SHALL be identical whether or not the deferral was used: deferring SHALL change only when the work happens.

A flush SHALL be able to record into the same delta record the stamps used, so a deferred stroke's undo restores shading as well as position. Nothing SHALL flush on the host's behalf, because the library does not know where a host-driven stroke ends.

#### Scenario: The final state is exact either way
- **WHEN** a host applies the same stamp sequence twice, once deferring and flushing at the end and once not
- **THEN** the resulting normals are identical

### Requirement: A host can see what its spatial index is worth
The C ABI and pyclay SHALL report the spatial index's measured partition quality, its leaf count, and whether the engine considers a rebuild warranted.

Whether a rebuild is warranted SHALL remain the engine's measurement and SHALL NOT be an instruction: queuing one SHALL require both that measurement and the host's own declared permission, and the two SHALL be separately observable.

#### Scenario: The host's permission alone withholds the job
- **WHEN** a host asks for a rebuild to be queued with a profile that forbids index rebuilds
- **THEN** nothing is queued, whatever the engine's measurement says
