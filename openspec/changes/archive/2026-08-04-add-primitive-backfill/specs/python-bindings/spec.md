# python-bindings — the full primitive set

Delta for `add-primitive-backfill`.

## ADDED Requirements

### Requirement: Every tape primitive has a Python class
The module SHALL expose a class for every primitive the tape can express, with the same placement keywords as the existing ones (`position`, `rotation_axis_angle`, `scale`) and parameter names matching the kernel's documented meaning.

#### Scenario: Full primitive coverage
- **WHEN** a test enumerates the module's primitive classes and adds one of each to a document
- **THEN** every one evaluates, meshes, and round-trips through `.clayspace`
