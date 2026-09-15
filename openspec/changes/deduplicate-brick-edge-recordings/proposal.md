## Why

Issue #531 still has a substantial meshing floor after exact grab-chain batching. A temporary phase probe on the host-configured sphere attributes about 17–18 ms to serial welding, versus about 7 ms to parallel marching. The recorder repeats lattice edges across tetrahedra, sending each repetition through the global vertex map.

## What Changes

- Record each ordinary brick-local lattice edge once using a bounded dense lookup.
- Retain the existing global welder, boundary attribution, vertex arithmetic, normal calculation and output order.
- Keep a general recording fallback for unsupported dimensions or edge coordinates.
- Add exact-output regressions and representative before/after meshing measurements.

## Impact

CPU brick meshing at both supported LOD levels. No public API, persisted data, device kernel, normal approximation, or document edits. This is a further contribution to #531, not a claim that all host gestures meet 16 ms.
