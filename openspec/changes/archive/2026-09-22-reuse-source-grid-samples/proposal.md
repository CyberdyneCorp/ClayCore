# Reuse source-grid samples

## Why
Smooth/Relax initialization evaluates identical halo positions in neighboring bricks. Issue #531 still exceeds the 16 ms application budget. A controlled 32-item source probe reduced first full materialization from 216.65 to 160.83 ms with identical volume bytes.

## What Changes
- Evaluate each unique position once for sufficiently large, complete uncomposed source grids.
- Scatter the results into the existing brick sample layout.
- Preserve the existing partial-window and composed-prefix paths.
- Add exact parity and evaluation-count regressions, and measure the current application baseline.

## Capabilities
### Modified Capabilities
- `sdf-prefix-cache`: exact full-grid source sampling with shared boundary evaluations.

## Impact
Private session sampling implementation only; no public API, file format, lattice or brush behavior changes. This is incremental work toward #531; completion still requires the application target across brushes.
