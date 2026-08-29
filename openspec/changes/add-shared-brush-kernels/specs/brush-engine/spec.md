# brush-engine — the brush model

Delta for `add-shared-brush-kernels`.

## ADDED Requirements

### Requirement: A brush is composed from orthogonal policies
The library SHALL express a brush as a composition of named, independent axes rather than as a verb with a settings struct: a stroke preset, a footprint, a weight model, a reference frame, a deformation kernel, an accumulation rule, a write target and a post policy.

Artist-facing brush families — ClayBuildup, DamStandard, hPolish, Trim Dynamic, Snake Hook, Rake — SHALL be expressible as values of those axes and SHALL NOT require an engine path of their own. A new named brush that needs a new code path is evidence an axis is missing, and the axis is what SHALL be added.

The axes SHALL be enums and plain data, not polymorphic objects. A per-vertex loop that dispatches virtually cannot be specialized, serialized or mirrored across a C ABI, and all three are required of this model.

The reference frame SHALL be explicit. `draw` displacing along the region's averaged normal and `inflate` along each vertex's own normal SHALL be the same kernel under two frames, and the results SHALL be identical to the two separate verbs they replace.

#### Scenario: A named brush family is a preset
- **WHEN** a preset library entry for a named brush family is loaded and applied
- **THEN** it resolves to axis values over existing kernels, and no kernel exists whose only caller is that one family

#### Scenario: Draw and inflate remain distinct under one kernel
- **WHEN** the same kernel is applied under the region-normal frame and under the vertex-normal frame to a region straddling a saddle
- **THEN** the two results differ in the way `draw` and `inflate` differed before this change, bit for bit

### Requirement: A stroke compiles its preset once
The library SHALL validate and compile a brush preset into a flat runtime plan at the beginning of a stroke, and each stamp SHALL read that plan rather than re-inspect the preset.

The plan SHALL record what the kernel actually needs — normals, neighbours, a pre-stamp snapshot, an alpha sampler, automask factors — so that a stamp gathers only what it will use.

A plan SHALL be cached against the preset's revision, and an unchanged preset SHALL NOT be recompiled between stamps.

#### Scenario: A plan is compiled once per stroke
- **WHEN** a stroke of many stamps runs with an unchanged preset
- **THEN** the preset is compiled exactly once and every stamp reads the compiled plan

### Requirement: Brush behaviour does not depend on event batching
Resolving the same stroke path SHALL produce the same stamps whether the host delivers its samples in one batch or in several, provided the stroke's transaction state is retained between calls.

Deterministic jitter, spacing, taper and clamped accumulation SHALL all hold under that split, because a host's event coalescing is a property of the device and not of the artist's gesture.

#### Scenario: One batch and five batches agree
- **WHEN** a stroke path is resolved as one batch of samples and then as five consecutive batches through one transaction
- **THEN** the resolved stamps are identical

### Requirement: Automasking gates a brush without a per-verb branch
The library SHALL provide automask factors — normal angle, topology connectivity, boundary proximity, cavity, and surface-group membership — evaluated over the brush's WORKSET and composed into the per-vertex weight by multiplication.

An automask SHALL be computed for the vertices a stamp reaches and SHALL NOT be computed, allocated or scanned for the whole surface.

Cavity and curvature automasks SHALL be derived from the SAME estimator the procedural mask verbs use, so that a painted cavity mask and a cavity automask cannot disagree about one surface.

The surface-group automask SHALL read the document's group field rather than a per-face group identifier. Groups are addressed on a world lattice so that they survive a representation bridge; a per-face copy would be a second answer to the same question and would not survive one.

A fully automasked vertex SHALL be bit-identical to its input position.

#### Scenario: A cavity automask and a painted cavity mask agree
- **WHEN** a cavity automask and a procedural cavity mask are evaluated over the same surface with the same parameters
- **THEN** they report the same values within the estimator's own tolerance

#### Scenario: An automask costs the workset, not the model
- **WHEN** a stamp with every automask enabled runs on a mesh of a million vertices with a footprint of a few thousand
- **THEN** the automask evaluation touches the workset and its read halo only

### Requirement: A brush preset is versioned and carries no image data
A brush preset SHALL carry a schema version from its first release, SHALL deserialize an older version by supplying defaults, and SHALL REFUSE an unknown newer version rather than interpreting the prefix it recognises.

A preset SHALL NOT embed alpha, height or displacement image bytes. Image content SHALL remain caller-owned and borrowed for the duration of a call, as the mesh alpha stamp already requires, so that a preset library costs kilobytes and a host owns its own resource cache.

#### Scenario: A newer preset is refused
- **WHEN** a preset serialized by a newer schema version is deserialized
- **THEN** the call fails with a typed error and produces no partially populated preset

#### Scenario: A preset round-trips
- **WHEN** a preset is serialized, deserialized and used to resolve a stroke
- **THEN** the resolved stamps are identical to those from the original preset
