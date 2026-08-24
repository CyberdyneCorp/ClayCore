# python-bindings

## ADDED Requirements

### Requirement: Document memory is reportable from Python
The Python bindings SHALL expose the document memory report and the per-layer report, with the same breakdown the C ABI reports.

The report SHALL be returned as a structure whose fields are readable by name rather than as a bare total, since the breakdown is what makes the figure actionable.

#### Scenario: A Python host reads the breakdown
- **WHEN** a document holding voxel content is asked for its memory
- **THEN** the per-subsystem figures are readable by name and sum to the total

#### Scenario: A Python host attributes a document to a layer
- **WHEN** a layer of a document is asked for its memory
- **THEN** the same fields are readable and the voxel content figure reflects that layer alone
