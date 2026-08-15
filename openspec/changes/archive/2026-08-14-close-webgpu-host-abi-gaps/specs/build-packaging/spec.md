# build-packaging — the parity fixture reaches a host's own tests

Delta for `close-webgpu-host-abi-gaps`.

## ADDED Requirements

### Requirement: The host parity fixture is reachable from the C ABI
The parity fixture SHALL be obtainable through the C ABI, not only by running the command-line tool. A host that consumes this library as a packaged framework runs its tests against that framework and cannot invoke a tool that is not in it, so a fixture reachable only from the CLI is a gate that host cannot run.

The bytes obtained through the ABI SHALL be identical to those the command-line tool writes, and SHALL be deterministic across calls within one build, so that a consumer can diff two runs and attribute any difference to a change it made.

The fixture SHALL carry what a consumer needs to gate its own evaluator without consulting this library again: the composed tapes, the probe points, this library's reference distance and colour at each probe, the tolerances to apply, and the safe step scale a sphere tracer needs.

#### Scenario: A host gates its preview from its own test bundle
- **WHEN** a consumer linking only the packaged library requests the fixture through the ABI and evaluates the same tapes with its own shader
- **THEN** it can assert agreement within the stated tolerances without invoking any tool outside its bundle

#### Scenario: The two producers agree
- **WHEN** the fixture is obtained through the ABI and written by the command-line tool from the same build
- **THEN** the two are byte-identical

#### Scenario: Repeated calls are diffable
- **WHEN** the fixture is requested twice in one process
- **THEN** the bytes are identical, so a difference between two runs is a change in the library rather than in the generator
