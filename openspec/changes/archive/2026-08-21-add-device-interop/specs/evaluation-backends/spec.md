# evaluation-backends — a backend can run on a device it did not create

Delta for `add-device-interop`.

## ADDED Requirements

### Requirement: A backend can be bound to a caller-supplied device
A GPU backend SHALL be constructible against a device, queue and context the caller already owns, as an instance the caller holds, distinct from the process-wide registered backend that creates and owns its own device. Registering a backend SHALL continue to mean what it means today, and SHALL NOT be affected by any device a caller supplies.

A backend that cannot adopt a supplied device SHALL report it as unsupported, on the same footing as any other capability it does not provide, and the caller SHALL be able to fall back to the registered backend and obtain identical values. Adoption changes where work runs, never what it computes.

A backend SHALL NOT create, destroy or wait on synchronization primitives belonging to the caller, and SHALL NOT submit work to a supplied queue outside a call the caller made. Work issued during a call SHALL be complete when that call returns, so no work is left in flight with no way for the caller to know when it lands.

Calls on one supplied device SHALL be the caller's to serialize, consistent with the brick cache's rule that the library adds no synchronization the consumer did not ask for.

#### Scenario: A supplied device computes what an owned device computes
- **WHEN** the parity suite is run through a backend bound to a caller-supplied device and through the same backend registered with its own device
- **THEN** the results agree within the GPU parity tolerance, and every capability reported supported in one is reported supported in the other

#### Scenario: A backend that cannot adopt says so
- **WHEN** a caller supplies a device to a backend that has no adoption path, or names an API whose backend is not built
- **THEN** construction reports unsupported, no device is retained, and the registered backend remains usable and unchanged

#### Scenario: The caller's queue is not driven behind its back
- **WHEN** a device-bound call returns
- **THEN** the work it issued on the caller's queue has completed, and the library holds no synchronization object the caller owns

### Requirement: Evaluation can write to device memory
Grid evaluation SHALL have a form whose destination is a caller-owned device buffer at a caller-chosen offset, so that results a consumer intends to draw from are produced in the memory it will draw from rather than copied through host memory.

The device form SHALL differ from the host form only in destination: same lattice, same cull-region semantics, same element order and same element type. Values SHALL cross as 32-bit floats and SHALL NOT be quantized on the device, because quantization and band classification are the cache's and duplicating them would create a second implementation of the step most able to drift.

The destination's available size SHALL be supplied and checked against the lattice, and a buffer too small SHALL be refused rather than partially written, consistent with every other call in this library that writes into a consumer's memory.

Brick evaluation SHALL reach the same destination through the same shape, each brick occupying its own fixed stride in one device buffer.

#### Scenario: Device output equals host output exactly
- **WHEN** the same grid and cull region are evaluated once into host memory and once into a device buffer on the same device
- **THEN** the two results are bit-identical, not merely within tolerance

#### Scenario: An undersized destination is refused
- **WHEN** a device buffer is supplied whose available size is smaller than the lattice requires
- **THEN** the call is refused and the buffer is not written

#### Scenario: A host preview never touches host memory
- **WHEN** a consumer evaluates bricks into its own device buffer and draws from it
- **THEN** the values do not pass through host memory at any point
