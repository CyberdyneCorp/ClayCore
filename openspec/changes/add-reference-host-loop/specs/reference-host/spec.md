# reference-host Specification

## ADDED Requirements

### Requirement: A runnable reference host session
The repository SHALL ship a `reference/` directory containing a scripted host
session that drives the Python bindings in the order an application would:
open a document, pick against it, edit under undo grouping, manage layers,
paint, then save and reload. It SHALL be executable standalone and SHALL exit
non-zero when any invariant it asserts fails.

#### Scenario: The session runs
- **WHEN** a user runs the reference host from a checkout with `pyclay` installed
- **THEN** it SHALL exit zero and print each phase it completed

#### Scenario: A broken invariant fails the run
- **WHEN** an operation in the session stops holding the property the session asserts
- **THEN** the run SHALL exit non-zero naming the phase and the property

### Requirement: The session covers the sequencing API
The session SHALL exercise every entry point whose meaning depends on ORDER
rather than on a single call, and which therefore cannot be shown by a gallery
page: undo grouping and its query surface, stroke trimming, layer reordering,
removal and protection, level dropping, plane and surface picking, selection
bounds, mesh containment and signed distance, mask cell painting, mirrored
painting, palette assignment, batch erase, stroke point appending, sculptor
refresh, and lattice identity.

#### Scenario: A sequencing entry point loses its only coverage
- **WHEN** the coverage check runs and an entry point named above is exercised by neither the gallery nor the reference host
- **THEN** it SHALL report that entry point as uncovered

### Requirement: The session states the order, not just the calls
Each phase SHALL record what a host is doing at that point and why the order
matters — which call must precede which, and what breaks otherwise — so the
file serves as the sequencing reference for a host written in another language.

#### Scenario: An implementor reads rather than runs it
- **WHEN** a reader follows the session top to bottom without executing it
- **THEN** each phase SHALL state its precondition and the consequence of getting the order wrong

### Requirement: The reference host runs in CI
CI SHALL execute the reference host and fail the build on a non-zero exit, on
the same bare `pyclay` + `numpy` + standard library environment the gallery
uses.

#### Scenario: A binding change breaks the session
- **WHEN** a rename or behaviour change breaks a phase
- **THEN** the CI job SHALL fail naming the phase that raised

### Requirement: No dependency beyond the wheel and numpy
The reference host SHALL import only `pyclay`, `numpy`, and the Python standard
library, and SHALL NOT open a window, start an event loop, or render.

#### Scenario: Bare environment
- **WHEN** it runs with only `pyclay` and `numpy` installed
- **THEN** it SHALL complete without an import error
