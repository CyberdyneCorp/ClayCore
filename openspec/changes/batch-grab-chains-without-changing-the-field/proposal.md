## Why

Issue #531 attributes the remaining chain-dependent brick-mesh cost to field
gradient normals. Replacing them with lattice normals was rejected because it
changes hard-edge normals by up to 78 degrees. The CPU blocked evaluator still
walks and decodes an entire deformer chain separately for every point.

## What Changes

- Apply compatible grab chains one deformer at a time across a point block,
  retaining the original operation order for every point.
- Hoist grab constants out of the point loop while preserving scalar arithmetic,
  all easing curves, front gating, and the exact four gradient taps.
- Keep the existing general evaluator for incompatible chains/repetition.
- Add bit-identity regression coverage and benchmark actual meshing and live-drag
  paths, including shallow controls and hard surfaces.

## Impact

CPU point, grid and batch evaluation; no public API, kernel opcode, document
representation, or normal-mode changes. This addresses measured engine work;
host latency and first-touch cost must remain separately attributed.
