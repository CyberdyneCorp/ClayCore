# c-abi — the brush model a host carries across representations

Delta for `add-shared-brush-runtime`.

## ADDED Requirements

### Requirement: One brush descriptor means one brush on every representation it is passed to
Where the C ABI documents a descriptor as being shared between representations — as `clay_dynamic_sculptor_stamp` documents `clay_mesh_brush_desc` — every field of that descriptor SHALL have the same effect on every entry point that takes it, or the entry point SHALL refuse the call.

An entry point SHALL NOT accept a descriptor field and ignore it. A host that sets the automask fields and observes no automask cannot distinguish "this surface had nothing to mask" from "this call does not implement it", and the descriptor's own documentation is what led the host to expect otherwise.

#### Scenario: The automask fields reach the adaptive path
- **WHEN** `clay_dynamic_sculptor_stamp` is called twice on the same surface with identical descriptors except that one sets `automask_factors` and the other sets zero
- **THEN** the two reports differ

### Requirement: A brush carries the stamp's azimuth
`clay_mesh_brush_desc` SHALL carry the stamp's azimuth in radians, appended under the existing `struct_size` rule so that a host compiled against the previous minor is unaffected and behaves exactly as it did.

Zero SHALL mean no rotation and SHALL be the default a `*_defaults` call produces, so that every existing host's stamps are unchanged bit for bit.

#### Scenario: An older host is unaffected
- **WHEN** a host compiled against the previous ABI minor passes a descriptor whose `struct_size` predates the azimuth field
- **THEN** the call succeeds and the stamp is identical to the one that ABI minor produced

### Requirement: A host can read what a sculptor's scratch costs
The ABI SHALL expose, for the fixed, adaptive and multiresolution sculptors, the capacity of the sculptor's scratch arena, its high-water mark and its growth count, through one descriptor filled by one call per sculptor kind.

It SHALL NOT expose a way to reserve, cap or tune that arena.

#### Scenario: The arena statistics cross
- **WHEN** a host queries a sculptor's arena statistics after a stroke
- **THEN** it receives capacity, high-water and growth count, and the growth count has stopped rising over stamps of similar footprint
