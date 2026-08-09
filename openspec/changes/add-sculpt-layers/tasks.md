# Tasks: add-sculpt-layers

- [ ] 1.1 DECIDE what brackets a pass, and whether it follows the undo-group precedent
- [ ] 1.2 Record a voxel pass as changed cells plus their previous values
- [ ] 1.3 Strength in [0, 1], dithered against the same cell-coordinate hash the falloff brushes use, so partial strength on binary occupancy is reproducible
- [ ] 1.4 Visibility, reorder, delete, merge down
- [ ] 1.5 State the memory budget and what happens when a document exceeds it
- [ ] 1.6 `.clayspace` persistence, backward-open so an older reader opens the document flattened rather than failing
- [ ] 1.7 Both bindings, C ABI additive
- [ ] 1.8 Tests: a pass at strength 0 leaves the grid identical to before it; at 1 identical to applying it directly; the same fractional strength gives the same cells on every platform; reordering two passes that touch the same cells is order-dependent and the test pins which order wins; round trip is bit-identical
- [ ] 1.9 DECIDE separately whether SDF sculpt layers are this feature or a weighted group, once expose-scene-groups has landed
