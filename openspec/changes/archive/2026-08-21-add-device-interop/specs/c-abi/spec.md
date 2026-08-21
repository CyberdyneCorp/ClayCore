# c-abi — a device crosses the boundary without a vendor header

Delta for `add-device-interop`.

## ADDED Requirements

### Requirement: A caller's device is adoptable across the ABI
The ABI SHALL let a consumer hand the library the device, queue and context it already owns, as an opaque handle the consumer creates and releases, and SHALL let evaluation run against that handle instead of against a device the library created.

Native objects SHALL cross as untyped pointers under a named graphics API, and `clay.h` SHALL NOT include, require or name any vendor header, since the header is read by bindings generators and its declared surface SHALL NOT vary with build configuration.

An API whose backend is not built, or a descriptor whose handles are incomplete for the API named, SHALL be refused when the device is adopted rather than at first use, so a consumer learns at the point it can still choose a fallback.

Adopting a device SHALL NOT change what the library computes. A consumer whose adoption is refused SHALL be able to use the registered backend and obtain the same values.

The header SHALL state that calls on one adopted device are the consumer's to serialize, as it already states for the brick cache.

#### Scenario: A host runs claycore on its own device
- **WHEN** a consumer adopts the device and queue it renders with, and evaluates through the resulting handle
- **THEN** the work runs on that device and the results match those the registered backend produces within the parity tolerance

#### Scenario: The header names no vendor type
- **WHEN** `clay.h` is parsed by a bindings generator with no graphics SDK present
- **THEN** it parses completely, and the set of functions it declares is the same one the built library exports

#### Scenario: An unbuilt API is refused at adoption
- **WHEN** a consumer names a graphics API whose backend was not compiled in
- **THEN** adoption fails with the reason available, nothing is retained, and the registered backends stay usable

### Requirement: Evaluation output can land in a caller's device buffer
The ABI SHALL expose grid evaluation and brick evaluation whose destination is a device buffer the consumer owns, described by an untyped handle, a byte offset and the size available from it.

The available size SHALL be required and checked against the lattice the call was given, and a destination too small SHALL be refused rather than partly written, consistent with this ABI never inferring a count.

The device form SHALL produce the same values as the host form for the same inputs, so that a consumer can verify one against the other.

#### Scenario: A brick refill never crosses host memory
- **WHEN** a consumer evaluates drained brick requests into its own device buffer
- **THEN** each brick occupies its own fixed stride in that buffer and no value is written to host memory

#### Scenario: A short device buffer is refused
- **WHEN** the size available from the offset is smaller than the results require
- **THEN** the call is refused and nothing is written
