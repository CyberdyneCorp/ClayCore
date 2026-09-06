## 1. Root cause

- [x] 1.1 Reproduce the issue's numbers and separate the surface from the shading: bisection of the composed zero set (exact at every cell size) against gradient-normal tilt (32° at cell 0.04, not shrinking) — the constant worst deviation is the raycast's stopping tolerance, the corrugation is the normals
- [x] 1.2 Rule the volume itself out: lattice samples exact to 1e-7, own-field tilt 0.51°, trilinear error shrinking quadratically

## 2. The feathered replace

- [x] 2.1 `feather` on `FieldVolume`, blob header 12 → 13 by the self-describing rule, and `clay_volume_params.feather` appended under the versioned-descriptor pattern, honoured by every producer that takes the struct
- [x] 2.2 `ccombine_replace_feather` in the kernel dialect: crossfade by box inset, correction clamped at the band, reading the volume's own blob header; every backend gets it from the shared interpreter
- [x] 2.3 Compiler emission: feather + Replace emits the new mode; feather zero emits the old one byte for byte; a truly empty chain degrades to hard while a cull-emptied one keeps the blend; the cull test widens by the band (`feather_cull_pad`); no mirror participation
- [x] 2.4 `cfi_replace_feather` declares the closed-form Lipschitz cost

## 3. The document-sourced relax

- [x] 3.1 `field::relax` source overload mirroring flatten's pair
- [x] 3.2 `clay_item_volume_relax_from` beside the in-place form, sharing its validation (`read_relax_settings`) and the one region convention (`read_volume_sampling`)
- [x] 3.3 Cross-reference from `clay_item_volume_relax`'s header note

## 4. Tests

- [x] 4.1 The round trip uncorrugates and CONVERGES: hard tilt > 4° at cell 0.04, feathered tilt < 2.5°/1.2° at 0.04/0.02 and falling, zero-set deviation O(cell²) and halving, safe step scale above a floor
- [x] 4.2 Feather zero pinned algebraically to `min(max(a, −b), b)` of the separately evaluated operands, exact equality
- [x] 4.3 The field outside the box is untouched (the hard replace's box capping is gone) and deep inside the box the result is the volume
- [x] 4.4 `relax_from` equals bake-then-relax exactly, plus the refusal set
- [x] 4.5 Blob and serialize round trips carry the feather; a pre-feather blob reads hard; the pre-Lipschitz-blob test updated for the taller header
- [x] 4.6 Per-brick culled tapes stay band-clamp bit-identical under a feathered replace, including the lone-volume seed cases

## 5. Docs and release hygiene

- [x] 5.1 `docs/07-brushes-and-features.md`: the round trip, the feather, the relax pair, the reachability table
- [x] 5.2 Bump to 0.28.0 in `CMakeLists.txt`, `bindings/c/clay.h`, `pyproject.toml`; record the addition in `docs/RELEASE.md`
- [ ] 5.3 Release gate (`release_check`, device gate) on the release branch
