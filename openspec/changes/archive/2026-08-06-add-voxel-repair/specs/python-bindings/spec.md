# python-bindings — repair

Delta for `add-voxel-repair`.

## ADDED Requirements

### Requirement: Repair from Python
The module SHALL expose the report and both repairs, with the report as a readable structure rather than a bare count.

#### Scenario: Reporting then repairing
- **WHEN** a script reports a hollow shell, fills its voids, and reports again
- **THEN** the first report says not airtight and the second says airtight
