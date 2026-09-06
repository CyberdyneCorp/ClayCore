## ADDED Requirements

### Requirement: A flatten can be sampled from a document
The C ABI SHALL provide a flatten whose source is a DOCUMENT rather than an existing volume, taking the flatten parameters, the sampling parameters (cell size, band, padding) and an optional region, and returning a new volume item.

Flattening a volume blends the plane with a source that is itself sampled, and the result declares a materially worse Lipschitz than flattening from an exact document does — measured at roughly 8x on the same shape, independent of the source volume's band. The surface is the same; what differs is what it costs to trace, and the engine's own raycast marches by `safe_step_scale`.

So where a document exists it SHALL be reachable as the source, because the cheaper field is available for the asking and the caller currently cannot ask for it.

#### Scenario: The sound path is reachable from C
- **WHEN** a host holds a document and wants a flattened volume
- **THEN** it can sample the flatten from that document in one call, without first baking a volume and choosing a band that its facet must not exceed

#### Scenario: The document-sourced field is cheaper to march
- **WHEN** the same flatten is taken from a document and from a volume baked from that document
- **THEN** both place the facet in the same place, and the document-sourced result reports a materially larger `safe_step_scale`; a test holds that difference rather than asserting only that both calls succeed

#### Scenario: The sampling parameters are the caller's
- **WHEN** a document-sourced flatten is requested with a cell size of zero or less
- **THEN** it is refused, because a document has no intrinsic scale to derive one from, exactly as `clay_item_volume_from_document` refuses it

### Requirement: Binding parity names one C symbol per operation
The binding-parity table SHALL NOT map two Python entry points with different operations onto one C symbol. Where a Python name has no C counterpart it SHALL be an explicit exemption carrying its reason, so that an asymmetry is visible in the table rather than concealed by it.

`Volume.flattened` and `Volume.flattened_from` are the case that motivated this: they take different sources, take different parameters, and differ in accuracy, and mapping both to `clay_item_volume_flatten` let the gap pass the gate.

#### Scenario: Two operations, two symbols
- **WHEN** the parity table is read for `Volume.flattened` and `Volume.flattened_from`
- **THEN** each names the C entry point that performs it, and neither stands in for the other
