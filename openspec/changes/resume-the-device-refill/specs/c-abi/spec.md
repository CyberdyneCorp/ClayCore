# c-abi

## MODIFIED Requirements

### Requirement: Evaluation output can land in a caller's device buffer
The ABI SHALL expose grid evaluation and brick evaluation whose destination is a device buffer the consumer owns, described by an untyped handle, a byte offset and the size available from it.

The available size SHALL be required and checked against the lattice the call was given, and a destination too small SHALL be refused rather than partly written, consistent with this ABI never inferring a count.

The device form SHALL produce the same values as the host form for the same inputs, so that a consumer can verify one against the other.

The device-destination brick refill SHALL resume on the same terms as the host-memory one: a brick whose seed can be carried forward exactly SHALL be answered from it rather than by walking the whole surviving edit list, and a brick that the device walked in full SHALL leave a seed behind for the next call. The seed store SHALL be the DOCUMENT's, so a brick seeded through either refill entry point can be served by the other.

Where the adopted backend cannot move bytes between host memory and the consumer's buffer, the refill SHALL walk every brick in full — the behaviour it had before it resumed anything — rather than partially resuming, failing, or producing a field a full walk would not produce. The decision SHALL be taken before anything is written.

Where a seed cannot be described exactly for a brick this path answered — in particular where more than one visible SDF layer means a seed is two values and this path evaluated the document whole — the refill SHALL store no seed for it rather than store one whose meaning it cannot state.

#### Scenario: A brick refill never crosses host memory
- **WHEN** a consumer evaluates drained brick requests into its own device buffer
- **THEN** each brick occupies its own fixed stride in that buffer and no value is written to host memory

#### Scenario: A short device buffer is refused
- **WHEN** the size available from the offset is smaller than the results require
- **THEN** the call is refused and nothing is written

#### Scenario: A stroke resumes into the caller's buffer
- **GIVEN** a device-destination refill has left seeds for a window of bricks
- **AND** one item is appended to the active layer
- **WHEN** the same window is refilled into the consumer's buffer again
- **THEN** the bricks carrying a usable seed are answered from it
- **AND** what lands in the consumer's buffer matches a full walk of the same document to the backend-parity standard

#### Scenario: A resumed run lands where it belongs
- **GIVEN** a window whose resumable bricks neither begin nor end the batch
- **WHEN** it is refilled into the consumer's buffer
- **THEN** every brick occupies the same fixed slot it would have occupied had none of them been resumed

#### Scenario: Seeds cross between the two refill entry points
- **GIVEN** the host-memory refill has stored seeds for some bricks of a window
- **WHEN** the device-destination refill asks for that window
- **THEN** those bricks are answered from their seeds and the rest are walked in full

#### Scenario: A backend that cannot move bytes still answers correctly
- **GIVEN** an adopted backend that reports no host-to-device copy capability
- **WHEN** a consumer refills bricks into its device buffer
- **THEN** every brick is walked in full and the values are what a full walk produces
