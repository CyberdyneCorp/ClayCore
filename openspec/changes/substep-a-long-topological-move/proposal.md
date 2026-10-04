## Why
`clay_item_volume_move_topological` sampled its source through one pull-back, `p - d·w(g(p))`. That map stops being one-to-one once `|d|` times the falloff's slope passes the radius. Past that point two output points read the same source point, so the surface under the grip sinks and the pulled material falls away before the end of the drag. The call still returned `CLAY_OK` (#657, found as ClaySpaceDesktop#177 F2, "Move Topological tears the surface").

The host fix (ClaySpaceDesktop#287) splits every drag into host-side steps of a quarter of the reach. Each step is a full `clay_item_volume_move_topological` call, so each one re-samples the whole volume.

The issue's probe, reproduced in-engine: a unit sphere baked at cell 0.01 over `[-0.4,-0.4,0.5]..[0.9,0.4,1.8]` with band 0.67, anchor `(0,0,1)`, radius 0.3, displacement `(0.5,0,0.4)` (`|d|` = 0.64), linear ease. The table gives the surface height along the drag.

| | x=0 | 0.05 | 0.10 | 0.15 | 0.20 | 0.25 | 0.30 | 0.35 | one call |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single step (before) | **0.939** | 1.265 | 1.246 | 1.193 | 1.125 | 1.037 | 0.953 | 0.936 | 388–418 ms |
| 5 slices, one call (after) | 1.116 | 1.150 | 1.185 | 1.221 | 1.258 | 1.294 | 1.327 | 1.365 | **361 ms** |
| same 5 slices as 5 host calls | 1.119 | 1.155 | 1.190 | 1.226 | 1.262 | 1.298 | 1.331 | 1.362 | — |
| host workaround, 9 calls of d/9 | 1.096 | 1.128 | 1.161 | 1.196 | 1.231 | 1.266 | 1.301 | 1.335 | 690 ms total |

The 9-call row matches the issue's own table to the third decimal. Timings are a Release `cpu-only` build on an Apple-silicon Mac, 5 repetitions × 2 runs.

The C ABI also had no document-sourced move. `field::move_topological` has the callable and `PointBatch` forms, and pyclay reaches them. A C host had to bake a volume first, with its band sized by hand to cover the drag. There was no counterpart to `clay_item_volume_flatten_from`.

## What Changes
- `field::move_topological` (all three overloads) runs a drag as `n = topological_move_steps(settings)` slices. `n` is the fewest that keep `|d/n| · ease_max_slope(ease) / radius ≤ 0.5`, capped at 64. Slice `i` drags `d/n` from `anchor + (i-1)·d/n`, and its geodesic is solved over the material that slices `1..i-1` left. Its material array is the source read through their composed pull-backs. The output is sampled once per point, at `pb_1(pb_2(…pb_n(p)))`. `n = 1` is bit-identical to the previous single step, and every drag in the existing tests and benchmarks is one slice.
- `field::topological_move_steps` is public, so a caller or a test can ask how a drag will be split.
- `ease_max_slope` moves from `scene/bounds.cpp` to `include/clay/math/ease_slope.h`, header-only, because `field` may not include `scene`. `scene::ease_max_slope` is now a using-declaration of it, so there is still one definition and no caller changes.
- C ABI 0.121.0 -> 0.122.0: `clay_item_volume_move_topological_from(doc, move, volume, region_min, region_max, out_item)`, modelled on `clay_item_volume_flatten_from`. It shares `read_volume_sampling` and samples through `eval::tape_point_batch`. When no region is passed, it samples the document's padded bounds grown by `|displacement|`.
- Both C move entry points refuse a non-finite anchor or displacement. The `clay.h` note on the in-place form explains the sub-stepping, the 64-slice limit, and when to use `_from` instead.
- `check_binding_parity.py` maps `Volume.moved_topologically_from` to the new `_from` entry point, which is its true counterpart.

## What building it found
- The 9-call reference in the issue is not the limit. 5, 9 and 17 host calls give 1.119, 1.096 and 1.086 at the anchor. Stepping converges as `n` grows rather than landing on one exact answer, so the regression test checks the property (never below 1.0, rising along the drag) and stays within 0.04 of the issue's table. It does not check one `n`'s numbers exactly.
- The sub-stepped call is cheaper than the single step it replaces. Each slice's geodesic grid is sized to `radius + |d|/n` rather than `radius + |d|`: 5 grids of about 94³ cells against one of about 196³. The output sampling, which dominates, is paid once either way.
- From a document at cell 0.02, the single step did not crater the crown. It left a lump over the anchor and clipped the far end of the pull. The C-level test therefore asserts that the pull arrives (`(0.35,0,1.25)` inside) and is a pull rather than a slab (`(0,0,1.25)` outside). Under a forced single step both assertions fail. Without the defaulted-region growth the first one fails.
- The step cap is not refused. A drag over 64 slices — over thirty radii on a linear curve — can fold again. The header says so rather than rejecting a gesture a host can produce.

## Impact
Drags within half the radius on a linear curve, and the equivalent for other curves, are bit-identical to before. Longer drags change shape: that change is the fix. One additive C entry point and no descriptor change, so `check_c_abi.py`'s struct mirror is untouched. No format change.
