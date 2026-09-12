## 1. The gap

- [x] 1.1 `clay_brick_cache_mesh_lod` refused gradient normals above lod 0, so a
      host drawing a coarse surface could only ask for face normals
- [x] 1.2 MEASURED against the field's own gradient on a worked sphere:
      CLAY_NORMAL_FACE 5,179 of 24,116 vertices over 5 deg, worst 84.78;
      CLAY_NORMAL_GRADIENT 0 over 5 deg, worst 0.00

## 2. Reading the refusal correctly

- [x] 2.1 Its reasoning is about the CULL, not the gradient: a coarse vertex
      sits off the field's surface, where a culled tape and the whole one are
      only both out-of-band rather than equal
- [x] 2.2 The whole document's tape is NOT band-clamped, so it has a real
      gradient at exactly that point — the flatness belongs to the cached
      lattice, not to the field

## 3. The change

- [x] 3.1 Gradient normals at a level are evaluated through the whole-document
      tape; the geometry is meshed without attributes and they are applied after
- [x] 3.2 lod 0 keeps the per-brick culled tapes unchanged (#73)
- [x] 3.3 Colours stay refused above lod 0, with the reason separated from the
      gradient's in both the header and the code

## 4. Two things refuted while building it

- [x] 4.1 REFUTED: "CLAY_NORMAL_FACE is area-weighted, so a sliver cannot poison
      it, so this change is unnecessary." compute_face_normals does accumulate
      the unnormalised cross product, and the normals are still 84.78 deg out
- [x] 4.2 REFUTED: excluding slivers from the accumulation. Scored before being
      proposed: 848 of 48,228 excluded, worst case 84.78 -> 84.78. The error is
      marching-cubes tessellation, not the degenerate triangles

## 5. Tests

- [x] 5.1 The lod-1 gradient mesh is scored against the sphere's ANALYTIC normal
      — the unit position — which the mesher cannot satisfy by accident and
      which face normals on a coarse lattice fail
- [x] 5.2 The comparison requires a non-zero sample, so a pass over nothing
      cannot read as a pass
- [x] 5.3 The colour refusal at lod 1 is pinned separately, so lifting one did
      not quietly lift the other

## 6. Still open

- [ ] 6.1 Device gate before any tag carries this
- [ ] 6.2 #549's other half: the brick mesher still emits the slivers. This
      changes what a host can ask for, not what the mesher produces
