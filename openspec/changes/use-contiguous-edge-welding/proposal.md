# Contiguous global edge welding

## Why

Issue #531 remains above its 16 ms brush-action target. Instrumented live previews spend about 30.6 ms in engine meshing. Global welding hashes canonical edge keys and allocates an unordered-map node for each first-seen crossing. A temporary contiguous-table prototype preserves fixture output hashes and reduces full mesh time by 3–10 ms.

## What Changes

Replace the temporary global edge lookup with a private, contiguous open-addressed table. Preserve canonical packed keys, complete key equality, first-encounter vertex numbering, interpolation arithmetic and traversal order. Test collisions, growth and exact mesh parity before adoption. Measure allocation and peak scratch-memory tradeoffs; retain the existing implementation if the memory/performance balance is unacceptable.

## Impact

Only transient meshing lookup storage changes. No public API, ABI, field arithmetic, document data or output ordering change is intended. This increment does not complete the 16 ms goal.
