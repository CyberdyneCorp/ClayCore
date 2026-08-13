# Tasks: quad meshing and quad export

## 1. The mesh carries quads

- [x] 1.1 `mesh::Mesh` gains `quads` (four indices per face) plus `quad_count()`
      and `has_quads()`; the header states the invariant — `indices` is the
      quads' triangulation, quad `q` at `indices[6q..6q+5]`, same positions.
- [x] 1.2 `mesh::quads_consistent(const Mesh&)` checks that invariant, so the
      stream reader and the tests assert it instead of restating it.
- [x] 1.3 `mesh::drop_quads(Mesh&)` — what an index-rewriting operation calls.
- [x] 1.4 `decimate` clears the quads; a decimated quad mesh is a triangle mesh
      and says so in the header.
- [x] 1.5 Audit every writer of `Mesh::indices` in the tree and make each one
      either preserve the invariant or clear the quads. List them in the commit.

## 2. The SDF quad mesher

- [x] 2.1 `detail::dual_grid_mesh` takes a "keep quads" option, filling `quads`
      from the `quad[4]` it already builds. Off by default, and the triangles
      and the 0-2 diagonal are unchanged in both modes.
- [x] 2.2 `mesh::mesh_tape_quads(tape, region, cell_size, options)` in a new
      `mesh/quad_mesh.h`, sharing `mesh_tape_nets`' sampler, apron ring and
      attribute application.
- [x] 2.3 The header states, first thing: lattice-derived quad grid, NOT
      field-aligned retopology; quads are non-planar; output is not manifold
      and not watertight, and marching cubes remains the export path for that.

## 3. The count search

- [ ] 3.1 `QuadTarget` (target, tolerance, max_iterations, min_cell_size) and
      `QuadFit` (cell_size, quad_count, iterations, within_tolerance, clamped).
- [ ] 3.2 `mesh_tape_quads_fit`: secant on `cell' = cell * sqrt(actual/target)`,
      capped, keeping the best seen, preferring the candidate that does not
      exceed the target when two tie.
- [ ] 3.3 The search never requests a lattice the resolution pricing would
      refuse; it stops at the ceiling and reports `clamped`.
- [ ] 3.4 A collapsing shape returns the best attempt with
      `within_tolerance = false` rather than the collapse.
- [ ] 3.5 The header states the granularity: count goes as `cell^-2`, so ±5-10%
      is the expectation, the default tolerance is 10%, and a much tighter one
      will exhaust the cap. And that each iteration is a whole mesh.

## 4. The voxel quad mesher

- [x] 4.1 Dual mode: the occupancy field `mesh_smooth` builds, sampled
      trilinearly so a cell size other than the voxel size works.
- [x] 4.2 GATE: at the grid's voxel size with `blur = 0`, dual mode's positions
      and indices are identical to `mesh_smooth`'s. Keep them one code path.
- [ ] 4.3 The cell-size clamp below the voxel size, reported through `QuadFit`.
- [x] 4.4 Faces mode: `sweep_window` gains an unmerged path (`w = h = 1`);
      `mesh_greedy` and `mesh_greedy_chunks` reach it never and are byte-identical.
- [x] 4.5 Faces mode welds by (lattice corner, palette index) and emits no
      vertex normals; both choices are commented with the failure they prevent
      (unwelded shells in a DCC; a rounded cube from averaged normals).
- [x] 4.6 Faces mode winding follows `emit_quad`'s flip, so the quad corner
      order and the triangles agree on which way the face points.
- [ ] 4.7 Faces mode's count lever is the multi-resolution level; a target picks
      the nearest, and the header states the ~4x granularity.

## 5. The exporters

- [x] 5.1 OBJ writes `f a b c d` with the same `v/vt/vn` spelling; a mesh with
      no quads produces identical bytes.
- [x] 5.2 PLY counts quads in `element face` and writes `4 a b c d`, binary and
      ascii both.
- [x] 5.3 FBX writes four indices per polygon with the complement end marker.
- [x] 5.4 GLB unchanged; `mesh_io.h` and the C header both say why (glTF 2.0 has
      no quad primitive mode).
- [x] 5.5 `mesh_io.h` states that the READERS still fan-triangulate, so a quad
      file re-imports as triangles.

## 6. The mesh stream

- [x] 6.1 `save_mesh_stream` appends a quad section — `u32 count` then four
      indices per quad — after the triangle indices, only when quads exist.
- [x] 6.2 `load_mesh_stream` reads the tail when bytes remain, bounding the
      count against them before allocating, as it already does for the rest.
- [x] 6.3 A malformed tail — bad count, index past the vertices, or not the
      triangulation present — refuses the stream as malformed.
- [x] 6.4 No mask bit, no `kClaySpaceMinor` / `kSceneMinor` move; the format
      notes record why (an older reader skips the tail instead of refusing the
      document).
- [x] 6.5 `save_mesh_stream` never writes a section its own loader refuses: a
      mesh whose quads are not the triangulation beside them serialises as the
      triangles it carries. The header records that the reader claims the FIRST
      tail, so a later section is appended after the quads.

## 7. The C ABI

- [ ] 7.1 `clay_quad_mode` (dual 0, faces 1) and `clay_quad_params` with
      `struct_size`, cell size, target, tolerance, iteration cap, mode.
- [ ] 7.2 `clay_document_mesh_quads` and `clay_voxel_mesh_quads`; faces mode on
      a document returns `CLAY_ERROR_INVALID_ARGUMENT` with a detail message.
- [ ] 7.3 `clay_mesh_quad_count`, `clay_mesh_quads`, `clay_mesh_copy_quads`
      (exact count required, as `clay_mesh_copy_indices` requires one).
- [ ] 7.4 `clay_quad_report` + `clay_mesh_quad_report`; a mesh that was not
      quad-meshed is refused rather than reported as zeroes.
- [ ] 7.5 `clay_mesh_transform` keeps quads; `clay_mesh_concat` and
      `clay_document_mesh_combined` carry them only when every input has them.
- [ ] 7.6 `clay_document_add_mesh_layer` copies quads; the borrowed mesh reports
      them.
- [ ] 7.7 The header states the retopology disclaimer at the quad entry points
      and the glTF exclusion at `clay_mesh_save`.
- [ ] 7.8 `tools/check_c_abi.py` passes; the ABI minor moves and the header
      records what it added.

## 8. Python

- [ ] 8.1 `Document.mesh_quads` and `VoxelGrid.mesh_quads`; string modes,
      unknown ones raise; faces mode on a document raises.
- [ ] 8.2 `Mesh.quads` as a zero-copy `(Q, 4)` uint32 view, empty `(0, 4)` when
      absent; `Mesh.quad_count`; `Mesh.quad_report`.
- [ ] 8.3 Docstrings carry the retopology disclaimer and the "approached, not
      hit" wording.
- [ ] 8.4 `tools/check_binding_parity.py` passes with no new exemption — every
      new member maps under the existing `clay_mesh_` / `clay_document_` /
      `clay_voxel_` prefixes.

## 9. Tests

- [x] 9.1 Invariant: every quad mesher's output passes `quads_consistent`.
- [x] 9.2 REGRESSION: every existing mesher returns byte-identical vertices and
      indices and an empty quad array — `mesh_tape`, `mesh_tape_nets`,
      `mesh_greedy`, `mesh_greedy_chunks`, `mesh_smooth`, the brick mesher.
- [x] 9.3 Dual quads: valence 3-6 averaging four on a sphere, no vertex in
      the interior of another face's edge and no edge shared by more than two
      quads (the T-junction check that is the reason greedy merging was
      rejected).
- [x] 9.4 The dual quad mesh's positions and triangles equal `mesh_tape_nets`'
      at the same cell size.
- [x] 9.5 Voxel dual at the voxel size equals `mesh_smooth` exactly.
- [x] 9.6 Faces mode: one quad per exposed face, welded within a colour, split
      across one, no normals, same covered surface as `mesh_greedy`.
- [ ] 9.7 The search lands within tolerance on a sphere and on a shape with
      thin features; the ceiling case reports `clamped`; an explicit cell size
      spends zero iterations.
- [x] 9.8 Export: OBJ/PLY/FBX carry four-corner faces; GLB carries triangles;
      a triangle mesh's bytes are unchanged in all four.
- [x] 9.9 Stream: quads survive a document round trip; a pre-quad reader's
      bytes still load; a corrupt tail is refused; a triangle mesh's bytes are
      unchanged.
- [ ] 9.10 Decimation drops quads; transform keeps them; mixed concat drops
      them; a mesh layer round trip keeps them.
- [ ] 9.11 asan/ubsan clean on the whole quad path, including the copy-out
      calls with a wrong buffer size.

## 10. Docs and gallery

- [ ] 10.1 `examples/44_quad_export.py`: an SDF document and a voxel sculpt,
      OBJ + PLY + FBX + GLB, printing requested against actual, with the
      retopology disclaimer in the header comment.
- [ ] 10.2 `examples/run_all.py` and the gallery README pick it up.
- [ ] 10.3 `docs/05-claycore-library.md` and `docs/08-mesh-readback.md` describe
      the quad path, the count contract and the glTF exclusion.
- [ ] 10.4 `docs/RELEASE.md`: the new ABI surface, the appended stream section
      that older readers skip, and — first, not buried — that this is a lattice
      quad grid rather than retopology.
