## Why

After normal batching, topology-heavy adaptive stamps still exceed the frame
budget. Collapse validation repeatedly walks the same two unchanged endpoint
neighborhoods and allocates temporary traversal buffers.

## What Changes

Reuse endpoint adjacency collected during collapse planning across the existing
link, duplicate-face, constraint and geometric checks. Preserve refusal ordering,
geometry arithmetic, constraints, operation budgets and recorded gesture output.
Measure current-main recorded strokes before selecting the final implementation.

## Capabilities

No new or modified capability requirements: implementation-only optimization,
with `skip_specs: true`. The dynamic-topology contracts remain unchanged.

## Impact

Private topology planning and regression tests. No public ABI, format or detail
policy changes. Baseline is current main `8a9d04e3`, including PRs #616 and #617.
