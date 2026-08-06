# c-abi — the cut tool

Delta for `add-cut-tool`.

## ADDED Requirements

### Requirement: Cuts across the ABI
The C API SHALL expose a versioned cut descriptor carrying the frame, the shape and the extent, resolving it into an item handle the caller places like any other.

#### Scenario: A cut means the same through both bindings
- **WHEN** a C consumer resolves a cut with a given frame and shape
- **THEN** the field matches what `pyclay` produces for the same cut
