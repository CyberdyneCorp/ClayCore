## Why
A painted mask is a thin volume around the source surface. Intersecting the extruded shell with that volume caps a requested wall at the paint depth. A 0.6-unit wall currently stops near 0.11 units.

## What Changes
- Evaluate the mask at the source surface under each sampled point so its footprint extends through the requested wall.
- Grow voxel extracts from masked surface cells for the requested number of layers, independent of mask depth.
- Measure wall height at multiple positions and thicknesses in regression tests.

## Impact
Mask extrudes produce their requested thickness for both field and voxel layers. Existing calls and saved document formats are unchanged.
