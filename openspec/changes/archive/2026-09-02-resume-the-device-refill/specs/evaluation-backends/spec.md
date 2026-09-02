# evaluation-backends

## ADDED Requirements

### Requirement: A backend can move bytes to and from a caller's device buffer
The backend interface SHALL offer a transfer of host memory INTO a slice of a buffer the caller owns, and a transfer of such a slice back OUT into host memory. Both SHALL move a byte count from the start of the slice, with no strides and no format conversion, and the transfer SHALL have completed when the call returns — the same rule every other device entry point in this interface follows.

Both SHALL default to `Unsupported`, and the capability report SHALL carry a flag, defaulting to false, saying which backends serve them. The flag exists so that a caller can decide BEFORE it commits to a plan that needs the transfers: a caller that discovered the refusal part way through a batch would have written some of its destination and not the rest, with no way back.

A backend that reports the capability SHALL touch the caller's buffer only through the binding or handle the caller's buffer was already required to support for evaluation, and SHALL NOT require the caller to have created that buffer with usage, flags or memory properties beyond what evaluating into it already requires.

Only a backend bound to a caller-supplied device can serve these; a backend that created its own device SHALL report the capability false and refuse.

#### Scenario: A backend without the capability says so and refuses
- **WHEN** a caller asks a backend that reports no copy capability to write to or read from a caller-owned buffer
- **THEN** both calls return `Unsupported` and nothing is written

#### Scenario: What is written is what is read back
- **GIVEN** a backend bound to a caller-supplied device
- **WHEN** host memory is written into a slice of the caller's buffer and the same slice is read back
- **THEN** the bytes read back are the bytes written

#### Scenario: An offset slice is honoured
- **WHEN** a transfer names a slice that does not begin at the buffer's start
- **THEN** only that slice is touched, and bytes outside it are unchanged
