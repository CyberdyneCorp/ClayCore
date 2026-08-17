# Tasks: decide-surface-colour

- [x] 1.1 DECIDE: is per-surface-point colour on an SDF layer a sampled colour field, or a non-goal?
      — NEITHER: it already IS one. `FieldVolume` carries optional per-sample RGB8 and
      `ctape_volume` reads it on every backend (`add-volume-color-channel`, shipped). The
      question the epic asked was answered before it was asked.
- [x] 1.2 DECIDE: does the voxel palette generalise to SDF layers?
      — NO, and it should not. 256 indexed entries is right for a representation whose
      material is quantised to cells and wrong for one already carrying continuous RGB per
      sample. The bridge between them is the conversion that exists: `rasterize_mesh`
      quantises INTO the palette, `to_field` carries it OUT as per-sample colour.
- [x] 1.3 DECIDE: PBR channels — paintable, or bake/export only?
      — BAKE/EXPORT ONLY, declared. Roughness and metallic are material authoring, which
      wants UVs and a texture set; UVs are CyberRemesherAndUV's half and
      `add-claycore-bridge` is where the baker's field-sampling callback belongs. A fourth
      channel beside RGB would quadruple every volume, reach every backend, and still not be
      paintable in the sense an artist means, because they mean a texture.
- [x] 1.4 Verify the claims the decision rests on against the build rather than the docs
      — Measured: a 24-sample Paint stroke appended 9 stamps and left the field
      BIT-IDENTICAL; colour at a painted point read exactly the authored `#e03020`; and
      consolidating at cell 0.02 (1113 bricks / 811k samples / 6.2 MB) left the colour at
      every probe unchanged.
- [x] 1.5 Pin the two properties the decision depends on as requirements: paint does not move the surface, and consolidation preserves colour
- [x] 1.6 Correct `docs/sculpt_comparison.md`, which said "no polypaint" in two places
- [x] 1.7 Record the one real gap the audit found, so it is a decision rather than an omission
      — COLOUR ON A MESH LAYER'S BRUSHES. `MeshSculptor` has fourteen verbs and every one
      moves vertices; nothing writes `Mesh::colors`. Blender's Paint and Smear are the
      missing pair, and a mesh layer can carry and export imported vertex colours but not
      have them edited — now the odd one out, since the SDF and voxel sides both paint.
      Pinned by a test that every verb leaves `colors` byte-identical, so a future colour
      brush adds colour writing deliberately rather than by accident.
