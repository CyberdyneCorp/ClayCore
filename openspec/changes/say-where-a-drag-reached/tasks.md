## 1. The gap

- [x] 1.1 `clay_layer_move_surface` answers a count; its live sibling
      `clay_sdf_move_update` hands back a dirty region
- [x] 1.2 The engine already builds one box per drag image and discards it
- [x] 1.3 A host reconstructs from brush size + distance because of that, and
      under a mirror a single box either misses an image or becomes the slab
      between them — "under a mirror that slab is the whole document"

## 2. The change

- [x] 2.1 `clay_layer_move_surface_regions`, six floats per box, min then max
- [x] 2.2 Both forms share one implementation, so the counting call cannot drift
- [x] 2.3 The capacity check lives in the shared path, beside the set it counts

## 3. Where the check goes, and why it is free

- [x] 3.1 The region set is final before anything is recorded: the dilation and
      the sharer loop are reads, `drag_frontier` takes a const document, and
      `prepare_frontier_seeds` is the first thing that records
- [x] 3.2 A short buffer is refused there, with the needed count, and nothing
      applied

## 4. What building it found

- [x] 4.1 The raw drag balls are NOT the right boxes. `apply_surface_gesture`
      dilates them into document space and widens them by the whole influence
      bound of every sharing layer; reporting the raw ones would have looked
      precise and been short

## 5. Tests

- [x] 5.1 THE ORACLE: sample the field before and after, and assert every point
      that changed lies inside a reported box. Requires the sample to have
      moved, so a drag that changed nothing cannot pass for the wrong reason
- [x] 5.2 A mirrored drag reports boxes straddling the mirror plane
- [x] 5.3 A short buffer refuses, reports the need, and leaves the field
      byte-identical — checked, not assumed
- [x] 5.4 Capacity 0 with no buffer asks the count without applying
- [x] 5.5 The counting form answers the same count and the same field

## 6. Still open

- [ ] 6.1 Device gate before any tag carries this
- [ ] 6.2 The other `clay_layer_*` surface gestures route through the same
      `apply_surface_gesture` and have the same gap
