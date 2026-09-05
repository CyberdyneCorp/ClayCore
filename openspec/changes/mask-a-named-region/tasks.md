## 1. The call

- [x] 1.1 `MaskField::fill_from_group`, driven by the group's extent
- [x] 1.2 Sampled at the mask's own cell centres, so the two cell sizes may differ
- [x] 1.3 Zero releases storage rather than writing zeros
- [x] 1.4 `kNoGroup` paints nothing and is not an error
- [x] 1.5 The C entry point, bracketed for undo exactly as every other mask
      mutation is
- [x] 1.6 pyclay: `MaskField.fill_from_group`

## 2. Gates

- [x] 2.1 The round trip is the same region — group -> mask -> group lands on
      the cells it started on, checked cell for cell and not by count
- [x] 2.2 The mask agrees with `clay_groups_at` point by point
- [x] 2.3 Zero erases: painted_count falls by exactly what was erased
- [x] 2.4 A fine mask over a coarse group quantises to the group
- [x] 2.5 `CLAY_NO_GROUP`, an unnamed id, and a null groups handle
- [x] 2.6 Version lines together; `check_*` green
