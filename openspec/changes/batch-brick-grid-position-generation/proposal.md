## Why

Issue #531 still spends substantial time preparing Smooth previews. Source fills call BrickGrid::sample_position for each sample, repeating brick-coordinate divisions 729 times per brick. An isolated exact-output probe reduces roughly 7.6–8.1 ms of coordinate generation to 0.9–1.5 ms by computing each brick base once. This is not yet an application latency result.

## What Changes

Add an exact bulk coordinate writer to BrickGrid and use it in SdfSourceField source fills. Derive each brick base through the existing sample_cell method and preserve global-cell float arithmetic, ordering and halo samples. Retain the scalar method as the reference. Keep sampling, prefix coverage, field evaluation and smooth semantics unchanged.

## Impact

Additive C++ helper and Smooth/source preparation only. No C ABI, document format, engine pin or preview lattice change. Add exact coordinate regressions and verify transaction samples and live latency before reporting gains.
