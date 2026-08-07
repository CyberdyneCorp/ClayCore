# build-packaging — parity fixture coverage

Delta for `extend-parity-fixture-coverage`.

## MODIFIED Requirements

### Requirement: Host parity fixture
ClayCore SHALL export a machine-readable parity fixture: a set of named cases, each carrying a composed tape (instructions, parameter block, out-of-line blob), a fixed set of probe points, and the CPU scalar reference distance and color at each probe, plus the tolerances a consumer should apply. It SHALL be reachable from the CLI (`clay parity-fixture -o <file>`) and SHALL be deterministic: two exports of the same build are byte-identical.

The case set SHALL cover the surface that a hand-written host preview gets wrong: every blend profile against smooth union, subtraction and intersection; every extended combine mode; the material-mix weights of a colored blend; a deformer chain; and at least one composed multi-layer document.

The case set SHALL TRACK THE KERNEL SET rather than a list fixed when the fixture was written. EVERY combine op the kernel implements SHALL have at least one case exercising it, checked against the op enumeration rather than against a list of names, so an op that ships without a case is a gate failure rather than a discovery. A feature that ships without one leaves the fixture reading as validation while asserting nothing about it, which is worse than absent coverage.

Deformer kinds and primitive families are NOT yet held to that standard: the case set exercises 5 of 14 deformers and 13 primitive opcodes, and the cases that would close those gaps are follow-up work. This is stated rather than implied so a consumer knows what a passing fixture does and does not cover.

Where a feature is a pair sharing one kernel branch with the sign or direction taken from the mode — relief and incise, magnify and pinch — the case set SHALL cover BOTH, since a backend can reproduce one and invert the other.

Each case's probe points SHALL reach the geometry that case exists to exercise. A case whose probes all sit far from its surface records agreement about empty space.

The fixture's own expectations SHALL be gated by the test suite against both the tape interpreter and the registered evaluation backends, so a fixture can never ship expectations that ClayCore itself does not reproduce.

#### Scenario: A host preview that mis-copies a blend fails the fixture
- **WHEN** a consumer evaluates the fixture's tapes with a quadratic smin of support `k` instead of `4k`
- **THEN** at least one probe disagrees with the recorded distance by more than the stated tolerance

#### Scenario: Fixture expectations match the engine
- **WHEN** the test suite evaluates each fixture case through the tape interpreter and every registered backend
- **THEN** the recorded distances and colors agree within the fixture's stated tolerances

#### Scenario: Export is deterministic
- **WHEN** the fixture is exported twice from the same build
- **THEN** the two files are byte-identical

#### Scenario: Every combine op is exercised
- **WHEN** the compiled tapes of the case set are scanned for the combine modes they use
- **THEN** every op in the kernel's combine enumeration appears in at least one case, and paired ops appear in one case per direction

#### Scenario: A case reaches its own geometry
- **WHEN** a case's recorded distances are examined
- **THEN** at least one probe lies near that case's surface rather than all of them in empty space
