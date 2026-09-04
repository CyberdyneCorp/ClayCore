## Why

Issue #455: a sampled volume answers a **distance** query 14x faster than the
primitives it was baked from and a **gradient** 9-20x slower, so meshing a bake
costs ~10x. Reproduced on `main` at `2ae87004`, larger than reported:

| field | gradient normals | face normals | bricks |
|---|---:|---:|---:|
| 97 primitives | 86.0 ms | 70.7 ms | 4,096 |
| 1 sampled volume | **1740.5 ms** | 79.7 ms | 6,859 |

Face normals cost the same on both, so it is not the meshing, the geometry or
the brick count — which is exactly what the reporter deduced from the same
symptom.

**The cause is that a per-brick culled tape copies the whole volume.** Gradient
normals are evaluated through per-brick culled tapes (`clay.h`,
`CLAY_NORMAL_GRADIENT`), and compiling one for a single 8^3 brick measures:

| field | culled compile | instrs | blob |
|---|---:|---:|---:|
| 97 primitives | 0.0056 ms | 3 (of 97 — the cull works) | 0 floats |
| 1 sampled volume | **0.3166 ms** | 1 | **1,243,861 floats** |

1.24 million floats copied so that 512 samples could be read, and
`0.3166 ms x 6,859 bricks = 2,171 ms` accounts for the whole of the observed
time. A volume's influence bound is its entire box, so the item cull — which
drops 94 of 97 primitives here — can never drop one, and nothing else narrowed
it.

## What Changes

- `field::FieldVolume::cropped(region)` — the sub-volume covering a region,
  and the tape compiler emits it in place of the whole payload whenever it is
  compiling against a cull region.
- **The origin and the brick grid do not move**, and that is the whole of why it
  is exact. The first implementation shifted the origin to the crop's first
  brick and the values came back differing in the last ulp — `-0.0598687`
  against `-0.0598688` — because the kernel reads a sample through
  `(p - origin) / cell` and a moved origin changes the trilinear weights.
  Keeping the grid costs almost nothing: `index_` and `far_` are one float per
  brick SLOT while `data_` is 729 per STORED brick, so the samples are
  **99.73%** of the payload and dropping only those keeps the arithmetic
  identical.
- Sound on the cull's own terms and only those. `ctape_volume_dist` CLAMPS a
  query onto the sampled box, so a cropped volume answers differently OUTSIDE
  the crop — which is the property a culled tape already has and already
  documents ("band-clamped results are bit-identical to the full tape inside
  the region").

## Result

| | before | after |
|---|---:|---:|
| per-brick culled compile | 0.3166 ms | **0.0054 ms** |
| that compile's blob | 1,243,861 floats | **27,160** |
| meshing a bake, gradient normals | 1740.5 ms | **142.9 ms** |

**12.2x**, and the volume now costs 0.0208 ms a brick against the primitives'
0.0214 — indistinguishable. The primitive row is unchanged (86.0 -> 87.7 ms)
and is the control, which matters because the after-run carried a load of ~10.

## Capabilities

### Modified Capabilities
- `scene-model`: a culled compile narrows a sampled volume to the region, as it
  already narrows the items.

## Impact

- `include/clay/field/volume.h`, `src/field/volume.cpp` — `cropped`.
- `src/scene/tape_build.cpp` — the emit site.
- `tests/unit/test_volume_cull.cpp`.
- No ABI change, no format change, no new entry point.
