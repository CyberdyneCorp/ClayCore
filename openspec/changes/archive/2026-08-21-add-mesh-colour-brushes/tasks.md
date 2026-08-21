# Tasks

## 1. The two verbs

- [x] 1.1 `MeshBrush::Paint` — blend toward `MeshBrushSettings::color` by the
      brush's own weight, so falloff, strength, geodesic, mask and alpha all
      compose without per-verb code
- [x] 1.2 `MeshBrush::Smear` — blend toward the one-ring neighbour most nearly
      opposite the drag, weighted by alignment
- [x] 1.3 A zero drag direction does nothing rather than degenerating into a
      smooth
- [x] 1.4 Neighbours inside the region are read at their PRE-STAMP colour, so
      a smear is simultaneous rather than dependent on class order
- [x] 1.5 `writes_color(verb)`, so "moves vertices" and "writes colour" is one
      question asked in one place
- [x] 1.6 A blend that is exact at both ends — `mix(a, b, 1)` is not `b` in
      floating point, and a fully-weighted dab would otherwise leave a one-ULP
      seam along every stroke's rim

## 2. The attribute, and undo

- [x] 2.1 `MeshSculptor::has_colors` / `ensure_colors(fill)`; the verbs refuse
      a mesh with no colour attribute rather than creating one
- [x] 2.2 `VertexDeltas` records colour as a third channel, exactly the way it
      already records normals — stored rather than recomputed
- [x] 2.3 `revert` and `apply` restore colour; `clear` drops it

## 3. Pin the properties

- [x] 3.1 A colour brush moves no vertex: `positions` and `normals` byte-identical
- [x] 3.2 The displacement verbs still leave `colors` byte-identical — the
      original claim, now scoped to the verbs it is still about
- [x] 3.3 A mesh with no colours is refused, and no attribute is created
- [x] 3.4 A full-weight dab lands on the target bit-identically
- [x] 3.5 Paint falls off from the centre and leaves the rim untouched
- [x] 3.6 Smear has a direction: the boundary moves with the drag, both ways,
      and a zero direction changes nothing
- [x] 3.7 A colour stroke reverts and re-applies bit-identically
- [x] 3.8 Both verbs are deterministic

## 4. Reach them from every binding

- [x] 4.1 `CLAY_MESH_BRUSH_PAINT` / `_SMEAR`, and `clay_mesh_brush_desc.color`
      appended after the alpha block
- [x] 4.2 `clay_mesh_sculptor_has_colors` / `_ensure_colors`
- [x] 4.3 pyclay: `'paint'` / `'smear'`, a `color=` argument on both the stamp
      and the stroke path, `has_colors` and `ensure_colors`
- [x] 4.4 Binding parity gate green (363 capabilities, up from 361)
- [x] 4.5 ABI 0.36.0 in all three places the release checklist names

## 5. Say what it does

- [x] 5.1 Spec delta on `meshing`: the new requirement, and the original
      colour requirement narrowed to the verbs it is still about
- [x] 5.2 `docs/sculpt_comparison.md` — the Blender/ZBrush parity rows
- [x] 5.3 An example that shows both verbs and the property that they move
      nothing
