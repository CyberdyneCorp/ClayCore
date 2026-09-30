## Implementation
- [x] Group coincident drag images once per drag (`image_balls`, `PreparedImage::leader`).
- [x] Resolve a group as its mean pull once plus each image's remainder (`resolve_prepared_move`).
- [x] Regression tests: the issue's measurement through the held call and the live transaction; one grab for a pull along the plane, the three-grab oblique split, a placed layer's rounding, a radial axis, and two grabs for images 1e-3 apart.
- [x] Resolve a coincident group as one magnify (`resolve_prepared_magnify`), with its regression test.
- [x] Keep grouping O(images) per frame (`PreparedImage::next`, the leader index), with a 256-copy radial test.
- [x] Correct the live-transaction test that asserted the doubled along-plane pull.
- [x] Update `move.h`, the `clay_sdf_move_preview_grab_count` note, `docs/05` and `docs/07`.
- [x] No ABI, format or version change: ABI stays at its current line.
