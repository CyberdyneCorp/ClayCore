# scene-model Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Document structure
`clay::scene` SHALL model a document as a list of layers, each `voxel` or `sdf` kind, with per-layer transform, visibility, resolution, and material. SDF layers SHALL hold an ordered edit list where each item applies to the combined result of all preceding items. Groups SHALL nest to depth ≥ 4 and carry group ops (including None). Layer instancing SHALL share content by reference such that editing the source updates all instances.

#### Scenario: Order matters
- **WHEN** an edit list [add sphere, subtract box] is reordered to [subtract box, add sphere]
- **THEN** evaluation produces a different field (subtract-before-add has nothing to remove), demonstrating ordered semantics

#### Scenario: Instance follows source
- **WHEN** a layer is instanced twice and an edit item is added to the source layer
- **THEN** both instances evaluate with the new item without duplicating stored content

### Requirement: Influence bounds
Every edit item and group SHALL expose a conservative influence bound: its shape AABB dilated by blend radius and rounding. The bound SHALL be conservative in the narrow-band sense that all evaluated storage relies on: outside the bound (dilated by the band width), band-clamped field values are unaffected by the item. (Raw far-field values may legitimately shift when a smooth-blend operand changes — smin deviates wherever |a−b| is inside the support width — which is why the guarantee, like brick storage, is stated band-clamped.)

#### Scenario: Bound is conservative
- **WHEN** a property test samples the field with and without an item at points outside the item's influence bound dilated by a band width β, clamping values to ±β
- **THEN** the two clamped fields are bit-identical at every sampled point

### Requirement: Blend locality guarantee
Because all blends are rigid, an edit whose influence bound does not intersect a region SHALL leave evaluated data (bricks, samples) for that region bit-identical. This SHALL be regression-tested at the brick level.

#### Scenario: Distant edit leaves bricks untouched
- **WHEN** a scene with a filled brick cache receives a new edit whose influence bound intersects none of a set of bricks
- **THEN** re-evaluation of those bricks produces bit-identical brick data

### Requirement: Tape compilation
The scene module SHALL compile an edit list into a flat postfix tape: opcode stream plus parameter blocks with transforms pre-inverted. Scenes SHALL NOT be compiled into shader source; every backend runs a fixed tape-interpreter kernel, so parameter edits never trigger kernel recompilation.

#### Scenario: Parameter edit is recompile-free
- **WHEN** an item parameter (e.g. sphere radius) changes
- **THEN** only the tape's parameter block is rewritten; the backend kernel binary is unchanged and re-evaluation can start immediately

#### Scenario: Tape round-trip fidelity
- **WHEN** any scene in the golden corpus is compiled to a tape and evaluated on the CPU reference
- **THEN** results equal direct tree evaluation within 1e-6

### Requirement: Per-brick tape culling
For brick evaluation the compiler SHALL emit per-brick tapes containing only the items whose influence bound intersects that brick (the Dreams design), preserving evaluation semantics exactly.

#### Scenario: Culled tape matches full tape
- **WHEN** a brick is evaluated with its culled tape and with the full scene tape
- **THEN** the brick data is bit-identical, and the culled tape length is ≤ the full tape length

### Requirement: Undo command vocabulary
Every document mutation SHALL be expressed as a serializable command with a computable inverse: add/remove/reorder item, set parameter, voxel-span edit, layer add/remove/reorder/retransform, group/ungroup. The in-memory undo stack and the document file format SHALL share this single command vocabulary. Consecutive commands from one stroke SHALL be coalescable into a single undo step.

#### Scenario: Command inverse restores state
- **WHEN** any command from the vocabulary is applied to a document and then its inverse is applied
- **THEN** the document state is bit-identical to the original (verified by serialization comparison)

#### Scenario: Stroke coalescing
- **WHEN** a sculpt stroke generates N incremental point-append commands followed by stroke end
- **THEN** undo removes the entire stroke as one step

