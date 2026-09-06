# Tasks: drag-a-squashed-item

- [x] 1.1 Reproduce it as a measurement rather than a reading of the code: a unit sphere scaled 3x on X, dragged at world (3, 0, 0) where its surface actually is — field 0.0 before, 0.0 after, against 0.0 -> 0.077 for the uniform case
- [x] 1.2 Map the grab CENTRE and the DISPLACEMENT through the per-axis scale, which comes off last because it is innermost
- [x] 1.3 Decide the RADIUS, which is a choice and not arithmetic: divide by the LARGEST factor, so every world reach is at most the radius circled. Under-reach is recoverable by dragging again; over-reach is not
- [x] 1.4 Put the two mappings in `scene/types.h` beside the other per-axis helpers, so the brush and the compiler cannot drift on which space a deformer runs in
- [x] 1.5 Three regression tests, each verified to FAIL against a revert that COMPILES: the drag reaches the surface; a stretched item drags identically to an unstretched one; the falloff never reaches outside what was circled
- [x] 1.6 Patch release 0.54.1 — one existing verb produces a correct result where it produced an inert one; no symbol added, no signature changed, no format change
- [x] 0.1 SEQUENCING: no format or ABI minor, so this runs ahead of the layer per-axis scale and that change builds on it

## Deliberately not in this change

- [ ] 2.1 **`brush::lattice_gizmo` has the identical bug** and is not fixed
      here, because it cannot be without a FORMAT change. The gizmo hands the
      deformer a `local_to_cage` placement and `Deformer::cage_xform` is a
      `math::Transform`; the map it now needs is `Transform ∘ diag`, which is
      not a Transform. Its own comment names the assumption that broke —
      "Rigid with uniform scale on both sides, so the composition is too." It
      belongs with the layer per-axis scale, which forces the same widening and
      takes a format minor anyway.
