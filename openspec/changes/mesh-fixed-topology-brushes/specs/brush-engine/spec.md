# brush-engine — the stroke engine's fourth consumer

Delta for `mesh-fixed-topology-brushes`.

## ADDED Requirements

### Requirement: Stamps apply to a mesh
The stroke engine SHALL gain `apply_to_mesh`, a fourth consumer of `resolve_stroke`'s stamps alongside `apply_to_grid`, `apply_to_mask` and `stamps_to_nodes`.

It SHALL take a mesh, its adjacency, the resolved stamps, the verb and its settings, and SHALL apply one stamp per resolved stamp, so spacing, pressure response, deterministic jitter, taper, steady stroke and buildup-versus-clamped accumulation reach mesh sculpting with no new machinery.

Each stamp's world radius and strength SHALL come from the stamp rather than from the settings, so pressure and taper shape a mesh stroke exactly as they shape a voxel one.

Under `Buildup` accumulation, overlapping stamps SHALL act repeatedly; under `Clamped`, the stroke SHALL reach its strength once however many stamps overlap. This is what makes one `clay` stamp into ClayBuildup.

`grab` and `snakehook` SHALL derive their per-stamp displacement from the motion between consecutive stamps, so a drag is a drag rather than a repeated identical pull.

It SHALL return the number of stamps that actually moved a vertex, and SHALL report the accumulated vertex deltas for the whole call as one coalesced record when the caller asks for it.

#### Scenario: A stroke inherits the engine
- **WHEN** a stroke is resolved with taper, jitter and a pressure curve and applied to a mesh
- **THEN** the stamps that reach the mesh are exactly the ones `resolve_stroke` produced, at their radii and strengths

#### Scenario: One stroke is one undo step
- **WHEN** a whole stroke is applied with a delta record requested and the record is reverted
- **THEN** the mesh is bit-identical to its pre-stroke state

### Requirement: A mask gates a mesh stroke
`apply_to_mesh` SHALL take a `voxel::MaskField` the same way `apply_to_grid` does: a stamp centred in a fully masked region SHALL be dropped, and each vertex's weight SHALL be scaled by `1 - mask` sampled at that vertex's world position.

The mask SHALL be sampled per VERTEX rather than per stamp, so a half-masked region under one stamp moves on one side and not the other.

This SHALL hold for every verb, including `grab` and `snakehook`, with no per-verb code.

#### Scenario: Half a region is protected
- **WHEN** the same stroke crosses a region whose far half is fully masked, for a displacement verb and for `smooth`
- **THEN** only the unmasked vertices moved, and the masked ones are bit-identical

### Requirement: Mesh strokes stay out of the tape
Nothing in `apply_to_mesh` SHALL enter a tape, a document evaluation, an edit list or the parity fixture. A sculpted mesh layer SHALL still never be evaluated, never blend with a field, and export exactly as its vertices say.

#### Scenario: The document evaluates to the same field
- **WHEN** a mesh layer in a document is sculpted
- **THEN** the document's evaluated field at every sample is unchanged
