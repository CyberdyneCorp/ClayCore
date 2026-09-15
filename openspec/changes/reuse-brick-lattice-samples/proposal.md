# Reuse brick lattice samples

## Why
Issue #531 still exceeds 16 ms for preparation and release. Updated component profiling measures median 7.42 ms recording ordinary cells, 9.54 ms welding, 5.50 ms collecting boundary cells and 2.87 ms in attributes. Ordinary cells repeatedly fetch shared corner values from the same immutable brick cache.

## What changes
For ordinary bricks with dimensions 1–16, read each point of the closed lattice once into a bounded local buffer and march through those exact values. Retain the general path for other dimensions and the independent reference recorder. Preserve mesh bits, ordering, attributes, boundary attribution and LOD behavior.

## Impact
Private meshing implementation and regression coverage only. Maximum sample payload is 17³ floats (19,652 bytes) per active worker. No persistent cache, public API, document format or field arithmetic changes. Full #531 completion still requires all reported actions to meet 16 ms.
