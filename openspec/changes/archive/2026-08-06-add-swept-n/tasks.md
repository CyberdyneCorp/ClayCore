# Tasks: add-swept-n

- [x] 1.1 Parallel transport along a tessellated guide, computed at compile time
- [x] 1.2 `ctape_swept` opcode: closest point on the guide, frame interpolation, arc-length profile bracketing
- [x] 1.3 `PrimType::Swept`; the guide reuses the curve point list, the profiles reuse the loft list
- [x] 1.4 Bounds from the guide's extent dilated by the widest profile
- [x] 1.5 Exactness and a curvature-driven Lipschitz; an overgrown profile degrades rather than failing
- [x] 1.6 Serialization; degenerate sweeps refused
- [x] 1.7 Both bindings; parity corpus row
- [x] 1.8 Tests: follows the guide, matches a capsule on a straight guide, interpolates, frame does not flip, tighter guide steps smaller, overgrown profile still compiles, round trip, guide point types matter, refusals, C-vs-Python
- [x] 1.9 Docs, example, ABI 0.19.0, full verification
