# examples Specification

## Purpose
TBD - created by archiving change add-examples-gallery. Update Purpose after archive.
## Requirements
### Requirement: Every feature area has a runnable example
The repository SHALL ship an `examples/` directory whose scripts collectively exercise the documented SDF vocabulary — primitives, blend kinds, extended combine modes, deformers, repetition, profile lifts, transitions and strokes — and the voxel sculpting surface — brush and box and line edits, mirroring, flood select, palettes, greedy meshing and SDF rasterization. Each script SHALL be executable standalone and SHALL write its outputs to `examples/output/`.

#### Scenario: Running an example produces artifacts
- **WHEN** a user runs any script in `examples/` from a checkout with `pyclay` installed
- **THEN** it exits zero and writes at least one PNG or model file under `examples/output/`

#### Scenario: A feature with no example is caught
- **WHEN** the gallery's coverage check runs
- **THEN** it reports any primitive class exposed by the module that no example instantiates

### Requirement: No dependencies beyond the wheel and numpy
Examples SHALL import only `pyclay`, `numpy`, and the Python standard library. Image output SHALL be produced by a bundled PNG writer rather than an imaging dependency.

#### Scenario: Bare environment
- **WHEN** the examples run in an environment with only `pyclay` and `numpy` installed
- **THEN** every script completes without an import error

### Requirement: Outputs are committed and reproducible
Rendered PNGs and exported models SHALL be committed under `examples/output/` so the gallery is viewable without running anything, and SHALL be regenerable by a single runner script.

#### Scenario: Regenerating the gallery
- **WHEN** a contributor runs the examples runner after changing a kernel
- **THEN** the committed outputs are overwritten in place, and the diff shows which features changed appearance

### Requirement: Examples run in CI
CI SHALL execute every example script and fail the build on a non-zero exit, so an API or behaviour change that breaks an example is caught in the same way a broken test is.

#### Scenario: A renamed binding breaks the gallery
- **WHEN** a Python binding is renamed without updating the examples
- **THEN** the examples CI job fails naming the script that raised

