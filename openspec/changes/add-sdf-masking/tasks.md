# Tasks: add-sdf-masking

- [ ] 1.1 Design note: how a mask-weighted offset forms a plate WALL, and the Lipschitz factor the result declares
- [ ] 1.2 Decide whether a mask is a document object with an id or a per-edit argument, and record why
- [ ] 1.3 Associate a `MaskField` with an SDF layer, addressed in world units as the voxel masks already are
- [ ] 1.4 Restrict an ordinary combine to the mask
- [ ] 1.5 Extrude the masked region by a thickness, outward or inward
- [ ] 1.6 Commands + undo inverses; `.clayspace` persistence if masks become objects
- [ ] 1.7 Both bindings, C ABI additive
- [ ] 1.8 Tests: a mask painted for a voxel layer means the same on an SDF layer; extrusion raises only the masked region; the wall's step scale is reported honestly; an empty mask is a no-op; parity across every registered backend
- [ ] 1.9 Example: a helmet panel raised from a mask instead of faked as a separate trimmed shell
