## ADDED Requirements

### Requirement: A host can stroke an adaptive surface across the ABI
The C ABI SHALL expose `clay_dynamic_sculptor_apply_stroke`, taking a stroke preset and a brush descriptor, and `clay_dynamic_sculptor_apply_preset`, taking a brush preset and an optionally borrowed alpha, each resolving the samples and applying them through the adaptive stroke consumer — so the drag anchoring, strength composition and azimuth a stroke means are the library's rather than each host's.

Both SHALL take samples as `clay_stroke_sample_full`, because the flat five-float packing carries no azimuth and a new entry point has no older stride to protect. Both SHALL take a topology descriptor, where NULL means the defaults, decoded by the same code `clay_dynamic_sculptor_stamp` uses; an optional mask; and an `orient_alpha_by_stamp` switch.

Both SHALL read the WORLD FRAME the handle declares and SHALL NOT take a per-call frame, so a stroke cannot spell its placement twice. Both SHALL accumulate a `clay_dynamic_stamp_report` over the stroke, honouring its `struct_size`: summed moved vertices and topology operations, the budget flag if any stamp hit it, the union of the dirty bounds, and the revisions after the last stamp.

A Layer brush SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` before any stamp runs. The header SHALL state what the calls do not provide: the stroke calls take no undo record (a `clay_dynamic_delta` is captured only per stamp, by `clay_dynamic_sculptor_stamp_recorded`), there is no normal deferral, and a stroke costs the sum of its stamps — measured at 1.004x the host's resolve-then-stamp loop over the same stamps.

Existing adaptive, fixed and multiresolution entry points SHALL keep their semantics unchanged.

#### Scenario: The ABI stroke equals the stamps it resolves
- **WHEN** a host applies samples through `clay_dynamic_sculptor_apply_stroke`, and separately resolves the same samples with `clay_stroke_resolve_full` and stamps each resolved stamp through the adaptive stroke's per-stamp settings on an identical surface
- **THEN** the two surfaces are identical and the accumulated report equals the sum of the per-stamp reports

#### Scenario: Layer is refused across the ABI
- **WHEN** a host calls `clay_dynamic_sculptor_apply_stroke` with a Layer brush
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT`, reports zero applied, and the surface revision is unchanged

#### Scenario: A brush preset's barrel reaches the surface
- **WHEN** a host applies the reference Rake preset through `clay_dynamic_sculptor_apply_preset` with an asymmetric alpha and `orient_alpha_by_stamp`, at two different stylus azimuths
- **THEN** the two surfaces differ

#### Scenario: A declared frame places the stroke
- **WHEN** a host declares a world frame on the adaptive sculptor and strokes in world coordinates
- **THEN** the surface changes where the same stroke in the surface's own coordinates changes it with no frame declared
