## Surface-anchored region
The mask describes which points on the source surface are selected. For a field sample p, estimate the source normal by centered differences, project p to the source surface using its signed distance, then evaluate the measured mask distance at that projection. Compose that region with the existing shell and rim rounding.

For a voxel grid, select masked surface cells once and grow into adjacent cells up to the requested layer count without reapplying the mask to each newly reached cell. Preserve palette indices through the existing colour remap.

## Verification
Measure outer wall height along several normals on a spherical cap at 0.05, 0.1, and 0.6 world units. Compare voxel and field extracts within one voxel and preserve the existing refusal, rounding, cancellation, and colour tests.
