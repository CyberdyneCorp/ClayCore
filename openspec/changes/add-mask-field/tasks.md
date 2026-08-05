# Tasks: add-mask-field

- [x] 1.1 `MaskField`: sparse chunked uint8 lattice in world units, sample/set at world positions
- [x] 1.2 Painting with the brush vocabulary (footprint, shape, falloff, strength)
- [x] 1.3 Region ops: invert, clear, expand, contract, smooth
- [x] 1.4 Optional mask on a layer; evaluation provably unchanged by its presence
- [x] 1.5 Masked voxel edits: effective strength scaled by (1 - mask)
- [x] 1.6 Serialization as its own document chunk; survives a resolution change
- [x] 1.7 Both bindings
- [x] 1.8 Tests: paint and read back, falloff is graded, invert round trip, frozen region survives an edit, partial masking attenuates, resolution-change survival, document round trip, C-vs-Python
- [x] 1.9 Docs; ABI 0.12.0; full verification
