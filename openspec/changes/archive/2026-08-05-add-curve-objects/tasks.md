# Tasks: add-curve-objects

- [x] 1.1 Point types (hard / spline / B-spline / Bezier) + local-space handles; closed flag; tolerance
- [x] 1.2 Adaptive tessellation to tolerance, bounded depth, deterministic
- [x] 1.3 Compiler tessellates into the stroke chain; all-hard-open is bit-identical
- [x] 1.4 Bounds from tessellated points
- [x] 1.5 A whole-list edit command, undoable and refused on a protected layer
- [x] 1.6 Version the scene chunk, threaded from the container minor
- [x] 1.7 Both bindings
- [x] 1.8 Tests: bit-identity, each point type, closed, tolerance/determinism/bound, undo, protection, bulge picking, round trip, older document, C-vs-Python
- [x] 1.9 Docs, example, ABI 0.15.0, full verification
