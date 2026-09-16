## Why

Issue #610. `VoxelGrid::sculpt_grab` passed `p.falloff` to `cgrab_point` as an
EASE index. The two enums do not line up — `BrushFalloff` is
Constant/Linear/Smooth/Gaussian = 0..3, `CEase` is
linear/smoothstep/smootherstep/in_quad = 0..3 — and `cregion_weight` applies the
ease to (1 - d), so every falloff delivered the NEXT one's curve. Sampled at
d = 0, 0.25, 0.5, 0.75:

    falloff     the name's curve            what the cast delivered
    Constant    1.000 1.000 1.000 1.000     1.000 0.750 0.500 0.250
    Linear      1.000 0.750 0.500 0.250     1.000 0.844 0.500 0.156
    Smooth      1.000 0.844 0.500 0.156     1.000 0.896 0.500 0.104
    Gaussian    1.000 0.755 0.325 0.080     1.000 0.562 0.250 0.062

All four still ran 1 at the centre to 0 at the rim, so a grab still tapered and
nothing looked broken. What it cost is the MEANING of the control: one
`BrushParams` field meant two different things depending on which verb read it,
and `Constant`'s own curve — a rigid pull inside the ball — was unreachable
through the grab at all.

## What Changes

- The grab weights its inverse map with `falloff_weight`, the same table every
  other voxel verb reads.
- Its structure stays `cgrab_point`'s term for term — weight, early-out at zero,
  front gate, same inverse map — so the two remain comparable. Only the weight
  function differs.
- Outside the ball is explicitly untouched, because `falloff_weight` clamps
  rather than gating and `Constant` would otherwise weight 1 everywhere.

## Impact

Voxel grab only. No ABI surface change, no new field, no document format: the
observable change is the curve a named falloff produces, which is the defect.
Nothing else reads the ease index.
