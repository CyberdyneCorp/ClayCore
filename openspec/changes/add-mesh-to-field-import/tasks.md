# Tasks: add-mesh-to-field-import

- [x] 1.1 `mesh::Bvh`: triangle BVH over a mesh, median split, nearest-first traversal
- [x] 1.2 Closest-point query giving unsigned distance
- [x] 1.3 Exact solid angle per triangle; generalized winding number
- [x] 1.4 Per-node aggregate dipole so far nodes are summarized rather than descended
- [x] 1.5 `mesh::to_field`: sample a mesh into a FieldVolume
- [x] 1.6 Python bindings: `Volume.from_mesh`
- [x] 1.7 C ABI: a volume producer; lift the construction refusal and both gate exemptions
- [x] 1.8 Tests: distance against analytic shapes, sign inside/outside, an OPEN mesh,
      a self-intersecting one, flipped normals, the approximation agreeing with the
      exact sum, and a round trip through a document
- [x] 1.9 Docs, example, full verification

Found while building, and done here rather than deferred:

- [x] 1.10 Neither the C ABI nor Python could LOAD a mesh — both could only
      save one — so the import had nothing to import. Added `clay_mesh_load`
      and `clay.load_mesh` beside the existing save, plus
      `clay_mesh_from_triangles` / `Mesh.from_triangles` for a caller that
      already has the geometry in memory.
- [x] 1.11 `MeshQuery`: the BVH exposed so a script can watch the sign behave —
      that a hole does not flip a half-space, that summarizing distant nodes
      moves nothing across the surface — rather than taking it on trust.
- [x] 1.12 The Swift smoke consumes the prebuilt xcframework, not the working
      tree, so it silently passed against a stale one. Rebuilt, and it now
      exercises the import end to end from Swift.
