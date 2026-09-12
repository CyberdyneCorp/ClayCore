## Why

Two findings from asking "is lazy mouse available on all brushes?" after #532
landed.

**Lazy mouse is not a per-brush feature**, and that context matters. `steady` is
applied once during stroke resolution — `clay_stroke_resolve` runs
`steady_path()` over the samples and returns smoothed stamps, so every brush
consuming stamps gets it for free. Move was the exception because it is a
**gesture** taking a total displacement from an anchor, and never passes through
resolution at all.

### 1. `clay_voxel_grab` has the same shape and the same hole

```c
clay_result clay_sdf_move_update   (clay_sdf_move_tx*,   const float total_displacement[3]);
clay_result clay_voxel_grab_update (clay_voxel_grab_tx*, const float total_displacement[3]);
```

Identical. Everything #532 argued about Move applies unchanged, and it had no
lag.

**The other gesture-shaped entry points do not need one**, recorded so nobody
adds an inert field for symmetry: `clay_sdf_smooth_update` takes no position, so
there is nothing to lag; `clay_multires_sculptor_begin_stroke` goes through
stamps, so resolution already covers it; `clay_layer_placement_begin` is a gizmo
rather than a brush.

### 2. The two paths disagreed on their ceiling

`steady_path` clamped to 0.95 silently. `clay_sdf_move_begin` refused only at
`>= 1.0`, so it accepted 0.99 — one named control, same units, **two
behaviours**.

## Not on clay_brush_params

That is the obvious place and it is wrong. `clay_brush_params` is shared by every
stamp-based voxel entry point, where a lag has nothing to act on because the
stamps arrive already resolved. A field accepted and inert on most of its callers
is exactly the failure mode `clay_move_params`' ignore-list was written against.

It goes on the **transaction**, scoped to where it acts.

## Refused rather than clamped

The gesture paths refuse above the ceiling; the stroke path keeps clamping.
Changing the stroke path to refuse would be more consistent with the house style
— "refused rather than clamped" appears throughout this header — but it changes
behaviour for existing preset callers, which is a separate decision and not
taken here. What matters is that a host can no longer set a value one path
honours and another quietly reduces.

## What changes for a caller

Additive: one new entry point. `clay_sdf_move_begin` now refuses a `steady`
above 0.95 where it previously accepted up to 0.99 — a range that has never
appeared in a published release, since `steady` itself ships in the same one.
