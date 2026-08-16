# Tasks: close-mesh-brush-vocabulary

- [x] 1.1 Alpha on `MeshBrushSettings`: samples, dimensions, and the frame (centre, direction, tangent, extent) it is projected through. Sampled by the kernel's `calpha_sample`, NOT a second bilinear lookup
- [x] 1.2 Fold the alpha into `BrushRegion::weights` during `gather`, so it composes with every verb and every falloff without per-verb code
- [x] 1.3 `MeshBrush::Relax`: the Laplacian target with its normal component removed.
      — MEASURED, and the number is not a constant, which is the part worth having: relax
      moves the surface 3.1x less than smooth at 22k triangles and only 1.7x less at 180k.
      Smooth's normal component scales with curvature times the SQUARE of the edge length,
      so the two converge as triangles shrink — relax earns most on a COARSE mesh, which is
      exactly the one that stretches visibly when topology cannot change.
- [x] 1.4 `MeshBrush::Layer`: deposit clamped to a ceiling above the stroke's starting
      surface, read from the `VertexDeltas` the stroke already keeps.
      — DECIDED: REFUSED without a record. Clamping against the current surface instead
      would make it draw under another name, and a verb that silently becomes a different
      verb is worse than one that returns 0.
- [x] 1.5 `MeshBrush::Nudge`: the drag projected into each vertex's tangent plane
- [x] 1.6 `default_geodesic` for each new verb, and whether each has a sign
      — Nothing needed changing: all three are local surface operations, so the existing
      rule (geodesic for everything except flatten and scrape, which mean "everything under
      this disc") already gives the right answer. Layer's sign is its height, which digs to
      a floor when negative.
- [x] 1.7 C ABI and pyclay surface, additive — three new enumerators and appended
      descriptor fields, every one of which reads ZERO as today's behaviour, which is what
      `struct_size` versioning requires
- [x] 1.8 Tests: no alpha is byte-identical to today; an all-zero alpha moves nothing; a mesh alpha and an SDF alpha agree on the same samples; relax lowers edge-length variance while moving the surface far less than smooth; layer stops at its ceiling where draw keeps going; nudge stays in the tangent plane; every verb leaves `indices`/`quads` byte-identical and reverts bit-exactly
- [x] 1.9 Example with a render: `examples/55_mesh_brush_vocabulary.py`.
      — RELAX IS DELIBERATELY NOT IN THE CONTACT SHEET, and the script says why: the
      previews resample the mesh into a field, which throws away the triangulation — the
      only thing relax changes. A render would show two identical spheres and imply the verb
      does nothing. The table is its evidence. Original tasking read: the case that earns relax is a region stretched by a big grab, and the case that earns layer is one slow stroke against one fast one
- [x] 1.10 Docs: `sculpt_comparison.md`'s surface-brush row and the brush table in `docs/07-brushes-and-features.md`
