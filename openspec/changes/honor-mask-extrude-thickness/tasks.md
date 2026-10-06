## Implementation
- [x] Project field samples to the source surface before reading mask distance.
- [x] Let voxel extracts grow beyond the painted mask volume.
- [x] Add measured height and evenness regression tests for both paths.
- [x] Update the API header and feature documentation.
- [x] Run focused C++ and C ABI regression tests.

## Restore the field path's cost (v0.126.0 device gate)
- [x] Take the projection's gradient from the sample lattice instead of six off-lattice taps.
- [x] Skip the projection where the shell alone decides the stored value, and prove the skip bit-identical to projecting every sample.
- [x] Assert the halo stays seamless across brick faces.
- [x] Gate source calls per lattice sample at <= 1.5 on the device fixture (7 before, 1.009 after).
- [x] A/B the C ABI on the device fixture against main and the pre-anchor engine.
- [x] Read the bound's finest level in place from the measured distances rather than copying them (peak memory).
- [x] Build an off-window neighbour's position through `BrickGrid::cell_position`, the function every sample position goes through.
- [x] Keep the unculled reference and a skip tally in a src-private header; assert the skip fires on whole bricks beyond the band and on stored samples under a painted interior.
- [x] Add a buried-mask fixture that catches a bound reading only half the projection's reach.
