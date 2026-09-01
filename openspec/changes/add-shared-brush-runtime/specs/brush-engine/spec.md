# brush-engine — a brush means one thing on every representation it is offered on

Delta for `add-shared-brush-runtime`.

## ADDED Requirements

### Requirement: Every part of a brush reaches every representation that offers the verb
Where a representation offers a verb, it SHALL honour EVERY factor of the brush that composes into the per-vertex weight — the falloff, the path taper, the freeze, the alpha, and the automask — and SHALL apply them in the one fixed order the composition rule states.

A representation that cannot honour a factor SHALL decline the verb, the way an adaptive surface declines Layer because half the vertices under the brush at the end of a stroke did not exist at its start. It SHALL NOT accept the brush and silently drop the factor. A brush setting that is accepted and ignored is worse than one that is refused: an artist who enables a gate and sees the surface move anyway has no way to tell a disabled gate from a gate that decided not to fire.

This requirement is written because the library did the ignored thing. The adaptive sculptor took a brush carrying automask settings, composed four of the five factors and dropped the fifth, on a path whose own C ABI documentation states that the descriptor is "the same descriptor the fixed path takes, so a host carries one brush model across both representations".

#### Scenario: An automask reaches the adaptive surface
- **WHEN** a stamp with the normal-angle automask enabled is applied to an adaptive surface across a fold
- **THEN** it moves fewer vertices than the same stamp with the automask disabled, and the vertices it declines to move are the ones facing away from the brush

#### Scenario: A factor that cannot be honoured is refused, not dropped
- **WHEN** a brush carries a factor a representation cannot compute
- **THEN** the call is refused with a reason, and no stamp is applied

### Requirement: The estimators a mesh module cannot compute are set once per stroke on every sculptor
The callbacks a mesh module structurally cannot compute for itself — the cavity measure, which is a field's Laplacian, and the surface-group field, which is a world lattice — SHALL be settable on EVERY sculptor that offers the automask, with the same signature, and SHALL be set once per STROKE rather than per stamp.

Per stroke is not a preference. They hold callable objects, and copying those per dab is an allocation per dab, which the allocation discipline forbids.

A sculptor that composes another — a multiresolution sculptor over a level sculptor — SHALL forward them, including to a level bound after they were set, so that changing the sculpt level mid-stroke does not silently drop them.

#### Scenario: Every sculptor takes the estimators
- **WHEN** a host sets the cavity and group estimators on the fixed, adaptive and multiresolution sculptors
- **THEN** all three accept them through the same call, and a stamp on each applies the cavity and surface-group factors

A STROKE RESOLVER THAT DRIVES A SCULPTOR SHALL WIRE THEM, and where no such
resolver exists for a representation the change SHALL say so rather than leave
the gap unnamed. `brush::apply_to_mesh` and `brush::apply_to_multires` wire
them from `MeshStrokeOptions`; there is no `brush::apply_to_dynamic`, so an
adaptive stroke is driven by the host calling `DynamicSculptor::stamp` directly,
and it is the host that calls `set_automask_inputs` — which it now can, and
before this change could not. Adding an adaptive stroke resolver is a larger
piece of work than this change is: it owns spacing, drag re-anchoring, the
snakehook anchor and the remesh schedule around every dab, none of which is
about the automask. What this change is responsible for is that the estimators
have somewhere to go on all three sculptors, and they do.

#### Scenario: Setting them allocates nothing per dab
- **WHEN** a stroke of many stamps runs after the estimators were set once
- **THEN** no stamp allocates on their behalf

#### Scenario: The adaptive path has no stroke resolver of its own
- **WHEN** a host drives an adaptive surface
- **THEN** it sets the estimators on the sculptor itself, with the same call the fixed sculptor takes, because there is no `brush::apply_to_dynamic` to do it on the host's behalf

### Requirement: A directional brush family is a preset over the shared frame
Rake, chisel, clay strips, a directional scratch and a rotated alpha SHALL be expressible as axis values over the shared stamp frame, and SHALL NOT require a frame, a sampler or a code path of their own.

The stamp's azimuth SHALL be part of the brush's settings, so that a stroke resolver which knows the direction of travel can orient the stamp without any verb knowing that it did.

The azimuth SHALL be carried by the brush preset format, at a schema version, as part of the brush's identity rather than of where a stamp landed. Nothing resolves an azimuth from a stroke's direction of travel yet, so a preset is the only place an artist can put one, and a library that dropped it would give a turned brush back unturned — the failure a version number exists to prevent. A record written by an earlier schema SHALL still load, taking the unrotated default it was in fact saved with.

#### Scenario: A directional family needs no engine path
- **WHEN** a directional preset is applied
- **THEN** it resolves to an azimuth on the shared stamp frame over an existing kernel, and no kernel exists whose only caller is that family

#### Scenario: A turned brush stays turned across the format
- **WHEN** a preset carrying a non-default azimuth is serialized and read back
- **THEN** it reports the same azimuth, and a preset carrying the default reports an exact positive zero

#### Scenario: A record from the earlier schema still loads
- **WHEN** a preset record written before the azimuth was carried is read
- **THEN** it loads with the unrotated default rather than being refused, while a record that is also truncated is still refused

### Requirement: A host can budget the memory a stroke's scratch will ask for
The library SHALL report, per sculptor, the capacity its scratch arena currently holds, the largest it has ever held, and how many times it has grown.

The reason is the same one the hierarchy's preflight gives: the device this library targets kills an application for memory rather than warning it twice, so a host that can see what a stroke's scratch costs can budget for it, and a host that cannot is guessing.

The library SHALL NOT expose a tuning knob for that arena — no reserve, no cap, no growth factor. Each would be a number a host tunes against one device and is then wrong about after a footprint change, and the arena already sizes itself from the largest footprint it has actually seen, which is the measurement such a knob would be guessing at.

#### Scenario: The scratch cost is reportable
- **WHEN** a host queries a sculptor after a stroke
- **THEN** it receives the arena's capacity, high-water mark and growth count

#### Scenario: There is nothing to tune
- **WHEN** a host looks for a way to preallocate or cap the arena
- **THEN** the API offers none, and the arena's capacity follows the footprints it has been given
