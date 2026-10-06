## Surface-anchored region
The mask describes which points on the source surface are selected. For a field sample p, estimate the source normal by centered differences over the sample lattice, project p to the source surface using its signed distance, then evaluate the measured mask distance at that projection. Compose that region with the existing shell and rim rounding.

For a voxel grid, select masked surface cells once and grow into adjacent cells up to the requested layer count without reapplying the mask to each newly reached cell. Preserve palette indices through the existing colour remap.

## Cost of the anchor
The first implementation estimated the normal with six off-lattice taps per sample, so every sample called the source seven times. On the v0.126.0 device gate (iPad15,5, iOS 27.0.1, against v0.120.1's engine) `mask_extrude` went from 51.6 to 288.3 ms at 10 stamps (5.58x), 374.5 to 2566.1 ms at 100 (6.85x) and 5297 to 26834 ms at 1000 (5.07x), with the growth exponent unchanged at 0.98. The gallery's one-shot `mask_extract` went from 75.3 to 468.5 ms (6.22x). Both device cases take the field path through `clay_document_mask_extrude`. The voxel path is not on the device gate.

Two changes bring the field path back to about one source call per sample:

- The gradient is a central difference over the sample lattice, using the distances one cell either side. The fill takes a whole window of bricks before projecting any of them. A neighbour across a brick face is read from the brick that sampled it, and only a neighbour outside the window calls the source. A lattice point is the same float position whichever brick names it, so samples shared by two bricks store the same bits.
- The projection is skipped wherever the shell alone decides the stored value. A max pyramid over the measured mask distances bounds what the mask could report at any point within |distance| of the sample. Where the shell exceeds that bound, max(shell, region) is the shell. With `border_round` the shell must exceed the bound by the quadratic blend's support (4k), where the blend term is exactly zero. A brick whose every shell is beyond the band is dropped by the volume regardless of the region. The skip is exact: the volume is bit-identical to projecting every sample.

The lattice gradient changes stored values against the six-tap version. On the test fixtures the largest change is 0.002-0.01 cells on smooth surfaces, about 0.13 cells at a box edge, and 0.21 cells for a 0.6 inward wall that reaches the sphere's centre, where the source has no gradient. The brick layout is identical on every fixture.

## Verification
Measure outer wall height along several normals on a spherical cap at 0.05, 0.1, and 0.6 world units. Compare voxel and field extracts within one voxel and preserve the existing refusal, rounding, cancellation, and colour tests.
