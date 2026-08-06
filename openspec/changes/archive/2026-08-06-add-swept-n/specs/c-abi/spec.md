# c-abi — sweeping along a guide

Delta for `add-swept-n`.

## ADDED Requirements

### Requirement: Sweeps across the ABI
The C API SHALL expose setting a swept item's guide points, reusing the curve point encoding, alongside the loft profile calls.

#### Scenario: A sweep means the same through both bindings
- **WHEN** a C consumer sweeps the same profiles along the same guide
- **THEN** the field matches what `pyclay` produces
