## ADDED Requirements

### Requirement: Stamps apply to an adaptive surface
The stroke engine SHALL provide `apply_to_dynamic`, a consumer of `resolve_stroke`'s stamps alongside `apply_to_mesh` and `apply_to_multires`, so spacing, pressure response, deterministic jitter, taper, steady stroke, accumulation, the stylus azimuth and brush presets reach an adaptive surface with no new machinery.

It SHALL apply exactly one `DynamicSculptor::stamp` per resolved stamp, with each stamp's radius and strength taken from the stamp and the rest of the brush from the settings — the same resolution of a stamp into brush settings that the fixed and multiresolution consumers use, shared rather than copied.

`grab` SHALL centre every stamp on the first stamp and drag by the motion between consecutive stamps, as `apply_to_mesh` does. `snakehook` SHALL centre every stamp on the surface VERTEX it is dragging, so the brush keeps up with the pull.

Because the adaptive surface retires vertex identities when it collapses an edge, a `snakehook` stroke SHALL revalidate its anchor before every stamp and, when the anchor no longer exists, SHALL re-find it as the vertex nearest the previous stamp's position, using the same nearest-vertex estimator the sculptor seeds its walk with. A stroke SHALL NOT keep stamping at a retired anchor's last position: measured, that reaches only 15–18% of a pull-out where the anchor dies often, and 41–88% where it dies one to three times.

A mask SHALL gate the stroke as it gates the other mesh consumers: a stamp centred in a fully masked region SHALL be skipped, and each vertex SHALL be weighed by the mask at its placed position. The cavity and surface-group estimators in the stroke options SHALL be wired once for the stroke. With `orient_alpha_by_stamp` set, each stamp's rotation SHALL orient the alpha; without it the alpha's tangent SHALL be exactly what the caller set.

It SHALL refuse, applying nothing and touching neither the surface, the record nor the sculptor's automask inputs: `MeshBrush::Layer`, which an adaptive surface does not offer; and a request to defer normals, which the adaptive sculptor cannot honour. A refusal SHALL NOT be a silent remap to another verb or a silently ignored option.

With a topology record given, the whole call SHALL accumulate into it as one reversible gesture. It SHALL return the number of stamps that changed the surface, and SHALL accumulate, when asked, the moved vertices, the topology operations, whether any stamp hit its operation budget, and the union of the dirty bounds.

#### Scenario: A stroke equals its stamps
- **WHEN** a stroke with taper, jitter and a pressure curve is applied to an adaptive surface through `apply_to_dynamic`, and the same resolved stamps are applied one by one through `DynamicSculptor::stamp` with the same per-stamp settings to an identical surface
- **THEN** the two surfaces are bit-identical, topology included, and the applied counts agree

#### Scenario: A snakehook keeps pulling
- **WHEN** a snakehook stroke pulls away from an adaptive surface
- **THEN** the surface follows the drag the way the same stroke through `apply_to_mesh` follows it on a fixed mesh, and not the way a loop centring each stamp on the cursor falls behind it

#### Scenario: A retired anchor is re-found
- **WHEN** the remesher retires the vertex a snakehook stroke is dragging
- **THEN** the next stamp re-finds the anchor nearest the previous stamp's position and the stroke keeps pulling, rather than stamping where the retired vertex was

#### Scenario: Layer is refused, not remapped
- **WHEN** a Layer stroke is applied to an adaptive surface
- **THEN** no stamp is applied, the surface and the record are unchanged, and the call reports zero

#### Scenario: The azimuth reaches the alpha on request
- **WHEN** two strokes that differ only in stylus azimuth are applied with an asymmetric alpha and a preset that turns the stamp to the barrel
- **THEN** with `orient_alpha_by_stamp` the surfaces differ, and without it they are bit-identical

#### Scenario: One stroke is one undo step
- **WHEN** a whole adaptive stroke is applied with a topology record and the record is reverted
- **THEN** the surface is bit-identical to its pre-stroke state and validates

### Requirement: The estimators a mesh module cannot compute are set once per stroke and wired by every stroke resolver
The callbacks a mesh module structurally cannot compute for itself — the cavity measure, which is a field's Laplacian, and the surface-group field, which is a world lattice — SHALL be settable on EVERY sculptor that offers the automask, with the same signature, and SHALL be set once per STROKE rather than per stamp.

Per stroke is not a preference. They hold callable objects, and copying those per dab is an allocation per dab, which the allocation discipline forbids.

A sculptor that composes another — a multiresolution sculptor over a level sculptor — SHALL forward them, including to a level bound after they were set, so that changing the sculpt level mid-stroke does not silently drop them.

A STROKE RESOLVER THAT DRIVES A SCULPTOR SHALL WIRE THEM. `brush::apply_to_mesh`, `brush::apply_to_multires` and `brush::apply_to_dynamic` wire them from `MeshStrokeOptions`, once per call. A host that drives any sculptor stamp by stamp instead sets them on the sculptor itself, with the same call on all three. Where the stroke options carry no estimator, the resolver SHALL leave whatever the host set on the sculptor in place rather than clearing it.

#### Scenario: Every sculptor takes the estimators
- **WHEN** a host sets the cavity and group estimators on the fixed, adaptive and multiresolution sculptors
- **THEN** all three accept them through the same call, and a stamp on each applies the cavity and surface-group factors

#### Scenario: Setting them allocates nothing per dab
- **WHEN** a stroke of many stamps runs after the estimators were set once
- **THEN** no stamp allocates on their behalf

#### Scenario: The adaptive stroke resolver wires them
- **WHEN** a host applies a stroke to an adaptive surface through `brush::apply_to_dynamic` with a cavity estimator in the stroke options
- **THEN** every stamp of the stroke applies the cavity factor, exactly as the same options do through `apply_to_mesh`

## REMOVED Requirements

### Requirement: The estimators a mesh module cannot compute are set once per stroke on every sculptor
**Reason**: One of its scenarios, "The adaptive path has no stroke resolver of its own", states the gap this change closes: `brush::apply_to_dynamic` now exists and wires the estimators. A MODIFIED block cannot drop a scenario, and keeping it would keep a false statement in the live spec.
**Migration**: Replaced by "The estimators a mesh module cannot compute are set once per stroke and wired by every stroke resolver", which carries the requirement's text and its two remaining scenarios unchanged and replaces the third with "The adaptive stroke resolver wires them".
