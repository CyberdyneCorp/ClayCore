## Why
A Move drag centred on a layer mirror plane and pulling along it was applied twice to an item straddling the plane (#663). The reflection of that drag IS the drag — same centre, same displacement — and the resolver gave the straddler one grab per reaching image, so the same grab twice. Measured on a unit sphere dragged from (0, 1, 0) by (0, 0.25, 0) at radius 0.35, linear ease:

| symmetry | surface lift at +y |
|---|---|
| none | 0.1458 |
| mirror X, before | 0.2309 (1.58x) |
| mirror X, after | 0.1458 |

Identical through the live transaction (`clay_sdf_move_*`) and the held call (`clay_layer_move_surface_regions`), because both resolve through `resolve_prepared_move`. A radial drag on its axis, pulling along it, had the same defect N times over (four grabs for a count of four). The host has no lever: the images are resolved inside the engine.

## What Changes
- Images of a drag whose centres coincide (within a ten-thousandth of the radius plus a millionth of the coordinates' magnitude, which absorbs a placed layer's rounding) are grouped when the drag is prepared. `PreparedImage::leader` names the first image of the group.
- A group resolves to the mean of its displacements as one grab, plus one grab per image for what it adds beyond that mean; components below a relative tolerance are dropped. A pull along the plane is exactly the unmirrored grab. A pull across it has a zero mean and keeps its two opposite grabs bit for bit, so the documented pinch is unchanged. An oblique pull applies the along-plane part once and pinches the rest (three grabs where there were two).
- Magnify resolves through the same prepared images and had the same defect: on the plane its reflection has the same centre and strength, so a straddler took the same magnify twice (a hard-seam mirror X lifted a unit sphere 0.006 a little off the centre, against 0.003 without). A group now resolves to one magnify, reaching when any member does.
- Grouping stays O(images) per frame: each group is a list threaded through `PreparedImage::next`, and the leaders are found through a projection-keyed index when the drag is prepared. A scan of every earlier image made a radial count of 4096 cost 19.5 ms to prepare and 5.1 ms per item per frame to resolve, against 0.2 ms and 0.52 ms before the merge; with the index they are 0.78 ms and 0.52 ms.
- `clay_sdf_move_preview_grab_count`'s header note, `docs/05`, `docs/07` and `include/clay/brush/move.h` state the rule, the measurement, and the discontinuity it leaves.

## What building it found
- The issue's preferred continuous fix (weight overlapping images by the max of their falloffs) is not a composition of grabs. Two composed half-grabs lift the same sphere 0.1600, not 0.1458, so no scaling of the existing grabs is exact on the plane. The continuous rule needs a multi-centre deformer in the kernel, on every backend, in the format and in the Lipschitz and bound code. That is a feature, not this fix.
- The resulting step is real and is documented: with the centre at x 1e-5 the lift is 0.1458, at x 1e-4 it is 0.2309, and the doubling fades continuously as the balls separate (0.2266 at 0.05, 0.2135 at 0.1, 0.1597 at 0.2).
- Dropping coincident images outright (the issue's other suggestion) would have fixed only the pure along-plane pull. An oblique pull's images differ in their across-plane parts, so they are not duplicates, and the along-plane part would still have been applied twice. Keeping only the first image would have brought back the one-sided pull the pinch test already rejects.
- `test_sdf_sculpt`'s "a live Move on the plane gives a straddler both grabs" asserted the defect. Its oblique drag carried the along-plane pull on both grabs. It now asserts three grabs with that pull on one of them.

## Impact
Only drags whose images coincide change: a drag on a mirror plane, or on a radial axis. Every other drag resolves bit-identically, because a group of one takes the old path unchanged. No ABI, format or version change.
