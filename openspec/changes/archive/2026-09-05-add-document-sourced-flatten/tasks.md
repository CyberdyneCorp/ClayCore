## 1. The entry point

- [x] 1.1 Declare `clay_item_volume_flatten_from` in `bindings/c/clay.h`, beside the in-place form, with a header note saying which to reach for and why
- [x] 1.2 Implement it in `bindings/c/clay_c.cpp`, reusing the flatten-half validation of `clay_item_volume_flatten` and the sampling-half validation of `clay_item_volume_from_document` rather than restating either
- [x] 1.3 Cross-reference from `clay_item_volume_flatten`'s own accuracy note, so a caller reading the limit is told where the unlimited path is

## 2. The gate that hid it

- [x] 2.1 Map `Volume.flattened_from` to the new symbol in `tools/check_binding_parity.py`
- [x] 2.2 Confirm the parity gate still passes and now names one symbol per operation

## 3. Tests

- [x] 3.1 A C ABI test that a document-sourced flatten succeeds and places the facet on the plane
- [x] 3.2 The discriminating test. NOT the one first written: both sources place the facet identically, which measuring disproved. What differs is `safe_step_scale` — about 8x, independent of band — so the test holds that, and holds that the surface is the same, which is what makes the steepness a cost rather than a trade
- [x] 3.3 Refusals: cell size <= 0, region_radius <= 0, zero-length normal, one of region_min/region_max without the other

## 4. Release hygiene

- [x] 4.1 Bump the ABI to 0.27.0 in `CMakeLists.txt`, `bindings/c/clay.h` and `pyproject.toml` (additive: one added symbol, nothing removed, no struct grown)
- [x] 4.2 Record the addition in `docs/RELEASE.md`'s version notes
- [ ] 4.3 Four presets green plus `release_check`
