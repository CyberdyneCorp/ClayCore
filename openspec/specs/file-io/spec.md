# file-io Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Document format (.clayspace)
`clay::io` SHALL read and write the `.clayspace` single-file binary chunked container: versioned chunks for scene commands (the undo command vocabulary), palettes, voxel grids (palette+RLE compressed), thumbnails (PNG), and camera bookmarks. Readers SHALL open any older format version (backward-open) and SHALL refuse newer major versions with a clear error (forward-refuse), never crashing or partially loading. The format lives entirely in claycore so Python and CI read/write projects without the app.

#### Scenario: Round trip
- **WHEN** any golden-corpus document is saved and reloaded
- **THEN** the reloaded document evaluates bit-identically (same tapes, same brick results) and serializes to identical bytes

#### Scenario: Forward refusal
- **WHEN** a file with a higher major format version is opened
- **THEN** loading fails with a version-mismatch error code and no partial document is produced

### Requirement: OBJ + MTL
The module SHALL provide a dependency-free OBJ reader and writer with MTL, supporting the documented vertex-color extension on export.

#### Scenario: OBJ export with colors
- **WHEN** a colored mesh is exported to OBJ
- **THEN** vertex colors are written per the documented extension and the file reimports with colors intact

### Requirement: FBX
The module SHALL import FBX via ufbx (meshes, transforms, vertex colors) and export via a minimal binary FBX writer producing meshes, transforms, and vertex colors with correct units and axis conventions for Unity, Unreal, and Blender. CI SHALL validate exports by round-tripping through assimp and headless Blender.

#### Scenario: Engine-correct axes and units
- **WHEN** a 1-meter cube authored in claycore is exported to FBX and imported into Blender headless in CI
- **THEN** it measures 1 meter with +Z up handled per convention mapping (no 100× scale, no rotated axes)

### Requirement: PLY
The module SHALL read and write PLY with vertex colors (binary and ASCII), interoperable with the SDF Modeler / MagicaCSG ecosystems.

#### Scenario: PLY color round trip
- **WHEN** a colored mesh is written to binary PLY and read back
- **THEN** geometry and per-vertex colors are preserved exactly

### Requirement: glTF/GLB writer
The module SHALL write glTF 2.0 / GLB (via cgltf or a custom writer) with meshes, vertex colors, and node transforms, validating against the glTF validator in CI.

#### Scenario: Valid glTF
- **WHEN** a golden scene mesh is exported to GLB
- **THEN** the glTF validator reports zero errors

### Requirement: Import guardrails
All importers SHALL enforce triangle/vertex budgets (configurable), survive malformed-file fuzzing without crashes or unbounded allocation, and fail with error codes — never exceptions across the ABI.

#### Scenario: Malformed file rejected safely
- **WHEN** a fuzzed/truncated FBX, OBJ, or PLY file is imported
- **THEN** the importer returns an error code with bounded memory use (no crash, no allocation bomb)

### Requirement: USDZ exclusion
claycore SHALL NOT implement USDZ. It SHALL expose mesh + attribute buffers in a layout directly consumable by platform USD APIs (e.g. Apple Model I/O in the app shell).

#### Scenario: Buffers ready for Model I/O
- **WHEN** a consumer requests mesh buffers for platform export
- **THEN** positions, normals, colors, and indices are exposed as contiguous typed arrays with documented layout requiring no per-vertex conversion

### Requirement: The scene chunk carries a version
The scene payload SHALL be decoded against the container's minor version rather than assuming the current layout, so that a field added to a node does not require a packing trick to stay backward compatible. A document written at an earlier minor SHALL load with the new fields at their defaults.

#### Scenario: An older document loads with hard corners
- **WHEN** a document written before point types existed is loaded
- **THEN** every stroke point is a hard corner, no list is closed, and the field is what it always was

#### Scenario: Curves round trip
- **WHEN** a document containing a closed Bezier curve is saved and reloaded
- **THEN** the control points, their types, their handles, the closed flag and the tolerance all come back, and the field is unchanged

