# Tasks: add-mesh-weld

- [x] 1.1 `include/clay/mesh/weld.h` + `src/mesh/weld.cpp`, routed through
      `Adjacency` so there is one answer to "are these the same vertex"
- [x] 1.2 Attribute splits preserved by default, so a UV seam survives
- [x] 1.3 Quads dropped on rewrite; byte-identical when nothing changed
- [x] 1.4 Every index in range afterwards, unconditionally
- [x] 2.1 C ABI: `clay_weld_desc`, `clay_weld_report`, `clay_mesh_weld_defaults`,
      `clay_mesh_weld`, with bounded fills
- [x] 2.2 The layer geometry revision bumped only when something changed
- [x] 2.3 pyclay `Mesh.weld`
- [x] 3.1 Tests: the marcher's zero-area triangles asserted as the premise;
      conversion before and after; the too-small-epsilon trap; the no-op
      byte-identity; the seam; quads; out-of-range indices; determinism
- [x] 3.2 C ABI and pyclay tests
- [x] 4.1 Correct `remesh-through-the-document`'s design note, which recorded
      `weld_epsilon = 0` as the fix
- [x] 4.2 The voxel remesh's own round-trip test welds instead
- [x] 5.1 Docs
- [x] 5.2 Version lines moved together
- [x] 5.3 `openspec validate --strict` clean
