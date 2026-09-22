## Why

Issue #531 still exceeds 16 ms. Current mesh phase probes attribute roughly
4–6 ms of a full sphere mesh to boundary-cell collection. Adjacent boundary
cells repeatedly perform the same cache lookup for their shared lattice points.

## What Changes

Cache boundary lattice reads lazily in bounded worker-local storage for ordinary
brick dimensions 1–16. Sample only requested points. Reset validity when the cell
owner changes. Preserve the original sampler for unsupported dimensions and the
reference recording path. Keep cell order, triangle attribution and output exact.

## Capabilities

No capability changes (`skip_specs: true`); private implementation optimization.

## Impact

Private meshing scratch helper, boundary recording, regression tests and timings.
No public API, ABI, geometry arithmetic or serialization changes.
