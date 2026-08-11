## ADDED Requirements

### Requirement: A volume placement can feather into the field beneath it
The sampling descriptor SHALL carry a feather width, appended under the versioned-descriptor pattern, honoured by every producer that takes the descriptor. A volume carrying a feather and placed with `CLAY_OP_REPLACE` SHALL crossfade over that margin: deep inside the sampled box the result is the volume's field alone, outside the box the surrounding field continues untouched, and the two blend across the feather just inside the box faces.

The hard replace holds both fields live at the surface, and a bake put straight back ties with the field beneath it at every sample plane, so finite-difference normals ripple at the cell wavelength however fine the cell — the corrugation of issue #67, whose zero set is exact while its shading is not. Holding ONE field almost everywhere is what removes it, and removes the hard box edge with it.

The blend's correction SHALL be clamped at the volume's band, so that the declared field slope stays closed-form (at most the operands' plus band times the weight's peak slope over the feather) and per-brick culling stays band-clamp exact. A surface the volume moved further than its band from the field beneath is therefore expressed only up to the band across the margin: the band at bake time is the knob that covers a deeper verb.

A feather of zero — and any descriptor sized before the field existed — SHALL be the hard replace, byte for byte. A feathered volume placed with `CLAY_OP_REPLACE` SHALL NOT participate in the layer mirror, because the crossfade follows one sampled box; every other op ignores the feather.

#### Scenario: The bake round trip converges instead of corrugating
- **WHEN** a document region is baked with a feather of about one band and added back with `CLAY_OP_REPLACE` with no verb applied
- **THEN** the composed field's gradient normals over the replaced surface tilt by an amount that SHRINKS as the cell shrinks, rather than sitting at a cell-independent ripple, and the zero set deviates from the source by no more than trilinear reconstruction error

#### Scenario: The box edge is no longer hard
- **WHEN** the composed field is evaluated outside the sampled box
- **THEN** it equals the field without the volume item exactly, rather than being capped by the volume's box distance

#### Scenario: Feather zero is byte-identical
- **WHEN** the feather is zero, or the descriptor's struct_size predates the field
- **THEN** the composed field equals `min(max(a, −b), b)` of the separately evaluated operands exactly, as it always has

### Requirement: A relax can be sampled from a document
The C ABI SHALL provide a relax whose source is a DOCUMENT rather than an existing volume, mirroring the document-sourced flatten: the same signature shape, the same sampling descriptor, the same optional-region convention, returning a new volume item.

Unlike the flatten pair there is no accuracy gap to close — relax moves the surface by less than a cell per pass, and a fresh bake's cell-aligned taps ARE the document at those lattice points — so the relationship to bake-then-relax SHALL be equality inside the band, not merely resemblance. What the entry point removes is the two-call round trip, and the volume-sourced path for hosts that have a document.

#### Scenario: One call equals the two it replaces
- **WHEN** the same relax is taken from a document in one call and by baking then relaxing in two
- **THEN** the two volumes evaluate identically inside the band; a test holds the equality exactly rather than approximately

#### Scenario: The refusals are the shared ones
- **WHEN** a document-sourced relax is requested with a cell size of zero or less, or with one of region_min/region_max without the other
- **THEN** it is refused with `CLAY_ERROR_INVALID_ARGUMENT`, exactly as `clay_item_volume_from_document` refuses it
