# c-abi

## ADDED Requirements

### Requirement: A long operation can be cancelled
The C API SHALL provide an opaque cancellation token that a host creates, passes to a long-running operation, cancels, and destroys.

Operations classed as an explicit user action rather than a frame — consolidating a layer, extruding a mask, relaxing, flattening or polishing a volume, and meshing a document for export — SHALL accept a token. Accepting a token SHALL be additive: a null token SHALL mean the operation cannot be cancelled and SHALL behave exactly as the same call behaves today, so an existing host is unaffected.

Cancellation SHALL be cooperative. The engine SHALL observe the token at bounded intervals of work and return early when it is set; it SHALL NOT abort a thread, unwind an exception across a worker, or leave the thread pool unjoined.

A cancelled operation SHALL return `CLAY_ERROR_CANCELLED`, which SHALL be distinct from every failure code and SHALL NOT set an error detail message describing a fault. A host SHALL be able to distinguish a user stopping an operation from the operation failing and from `CLAY_ERROR_BUDGET_EXCEEDED`, which means a limit the host declared before the call.

Cancelling a token SHALL be the ONLY operation on any handle that is safe to perform from a thread other than the one running the call, and the header SHALL say so explicitly, because the surrounding threading contract requires every other call on a handle to be serialized. A token SHALL be safe to cancel before an operation starts, during one, and after one has returned.

#### Scenario: A cancelled operation returns a distinct code
- **WHEN** a token is cancelled from another thread while a consolidate is running
- **THEN** the call returns `CLAY_ERROR_CANCELLED` rather than a failure code, and rather than `CLAY_ERROR_BUDGET_EXCEEDED`

#### Scenario: A null token preserves today's behaviour
- **WHEN** an operation that accepts a token is called with a null token
- **THEN** it runs to completion and produces a result bit-identical to the same call before tokens existed

#### Scenario: Cancelling before the call
- **WHEN** a token is cancelled and then passed to an operation
- **THEN** the operation returns `CLAY_ERROR_CANCELLED` promptly without doing the work

#### Scenario: Cancelling after the call
- **WHEN** a token is cancelled after the operation using it has already returned
- **THEN** the cancel is accepted, changes nothing, and the token can still be destroyed

### Requirement: A cancelled operation changes nothing
An operation that returns `CLAY_ERROR_CANCELLED` SHALL leave every object it was given exactly as it found them.

A cancelled operation SHALL NOT commit a partial result, SHALL NOT push an entry onto the undo stack, and SHALL NOT leave a document that evaluates differently from before the call. A host SHALL NOT have to undo a cancellation.

Where an operation writes into a caller-owned output — a mesh handle, a descriptor — a cancelled call SHALL leave that output unwritten rather than partially filled, and SHALL NOT transfer ownership of anything the host would then have to free.

#### Scenario: A cancelled consolidate leaves the layer parametric
- **WHEN** a consolidate is cancelled part way through the bake
- **THEN** the layer still holds its original edit list, the document evaluates identically at every probe point, and the undo depth is unchanged

#### Scenario: A cancelled operation frees nothing onto the host
- **WHEN** an operation that would return an owned handle is cancelled
- **THEN** no handle is written to the output parameter and the host has nothing to free

### Requirement: Progress is readable without a callback
The token SHALL carry progress written by the engine and readable by the host, so that no function pointer crosses the ABI.

Progress SHALL be readable through a versioned output descriptor carrying the current phase index, the phase count, a fraction within the current phase, and a done/total unit count where the operation has an honest one. The descriptor SHALL NOT carry a time estimate.

The fraction SHALL be monotonic within a phase. The phase index SHALL be monotonic across an operation. Reading progress SHALL be safe from a thread other than the one running the operation, and SHALL be safe when no operation is running, in which case it SHALL report that state rather than a stale value from a previous call.

An operation that reports no meaningful progress SHALL report a single phase rather than fabricating a fraction.

#### Scenario: Progress advances during a long operation
- **WHEN** a host polls a token from another thread while a mask extrude is running
- **THEN** the reported phase index and fraction advance, the fraction never moves backwards within a phase, and the final read before the call returns reports the last phase

#### Scenario: Progress on an idle token
- **WHEN** a host reads progress from a token no operation is using
- **THEN** the call succeeds and reports that no operation is in progress

#### Scenario: No callback crosses the boundary
- **WHEN** `clay.h` is processed by a standard bindings generator
- **THEN** the cancellation and progress surface generates with no function-pointer type, consistent with the FFI-general design requirement
