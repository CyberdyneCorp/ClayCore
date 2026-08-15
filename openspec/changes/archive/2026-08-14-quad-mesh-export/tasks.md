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

- [x] 3.1 `QuadTarget` (target, tolerance, max_iterations, min_cell_size) and
      `QuadFit` (cell_size, quad_count, iterations, within_tolerance, clamped).
- [x] 3.2 `mesh_tape_quads_fit`: secant on `cell' = cell * sqrt(actual/target)`,
      capped, keeping the best seen, preferring the candidate that does not
      exceed the target when two tie.
- [x] 3.3 The search never requests a lattice the resolution pricing would
      refuse; it stops at the ceiling and reports `clamped`.
- [x] 3.4 A collapsing shape returns the best attempt with
      `within_tolerance = false` rather than the collapse.
- [x] 3.5 The header states the granularity: count goes as `cell^-2`, so ±5-10%
      is the expectation, the default tolerance is 10%, and a much tighter one
      will exhaust the cap. And that each iteration is a whole mesh.

## 4. The voxel quad mesher

- [x] 4.1 Dual mode: the occupancy field `mesh_smooth` builds, sampled
      trilinearly so a cell size other than the voxel size works.
- [x] 4.2 GATE: at the grid's voxel size with `blur = 0`, dual mode's positions
      and indices are identical to `mesh_smooth`'s. Keep them one code path.
- [x] 4.3 The cell-size clamp below the voxel size, reported through `QuadFit`.
- [x] 4.4 Faces mode: `sweep_window` gains an unmerged path (`w = h = 1`);
      `mesh_greedy` and `mesh_greedy_chunks` reach it never and are byte-identical.
- [x] 4.5 Faces mode welds by (lattice corner, palette index) and emits no
      vertex normals; both choices are commented with the failure they prevent
      (unwelded shells in a DCC; a rounded cube from averaged normals).
- [x] 4.6 Faces mode winding follows `emit_quad`'s flip, so the quad corner
      order and the triangles agree on which way the face points.
- [x] 4.7 Faces mode's count lever is the multi-resolution level; a target picks
      the nearest, and the header states the ~4x granularity.

## 5. The exporters

- [x] 5.1 OBJ writes `f a b c d` with the same `v/vt/vn` spelling; a mesh with
      no quads produces identical bytes.
- [x] 5.2 PLY counts quads in `element face` and writes `4 a b c d`, binary and
      ascii both.
- [x] 5.3 FBX writes four indices per polygon with the complement end marker.
- [x] 5.4 GLB unchanged; `mesh_io.h` and the C header both say why (glTF 2.0 has
      no quad primitive mode).
- [x] 5.5 `mesh_io.h` and the C header state that the READERS are unchanged, so
      a quad file re-imports as triangles — and which triangles: OBJ and PLY
      keep the writer's diagonal, FBX does not.

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

- [x] 7.1 `clay_quad_mode` (dual 0, faces 1) and `clay_quad_params` with
      `struct_size`, cell size, target, tolerance, iteration cap, mode.
- [x] 7.2 `clay_document_mesh_quads` and `clay_voxel_mesh_quads`; faces mode on
      a document returns `CLAY_ERROR_INVALID_ARGUMENT` with a detail message.
- [x] 7.3 `clay_mesh_quad_count`, `clay_mesh_quads`, `clay_mesh_copy_quads`
      (exact count required, as `clay_mesh_copy_indices` requires one).
- [x] 7.4 `clay_quad_report` + `clay_mesh_quad_report`; a mesh that was not
      quad-meshed is refused rather than reported as zeroes.
- [x] 7.5 `clay_mesh_transform` keeps quads; `clay_mesh_concat` and
      `clay_document_mesh_combined` carry them only when every input has them.
- [x] 7.6 `clay_document_add_mesh_layer` copies quads; the borrowed mesh reports
      them.
- [x] 7.7 The header states the retopology disclaimer at the quad entry points
      and the glTF exclusion at `clay_mesh_save`.
- [x] 7.8 `tools/check_c_abi.py` passes, and the new surface is recorded in
      `docs/RELEASE.md`. The ABI minor is NOT moved here: `docs/RELEASE.md`
      requires the version to agree across `CMakeLists.txt`, `clay.h` and
      `pyproject.toml`, and this repository moves all three in one "Bump to
      X.Y.Z" release commit — a feature branch that moved the minor on its own
      would claim a release it is not. The unreleased notes name the six
      symbols, the two structs and the enum this change adds.

## 8. Python

- [x] 8.1 `Document.mesh_quads` and `VoxelGrid.mesh_quads`; string modes,
      unknown ones raise; faces mode on a document raises.
- [x] 8.2 `Mesh.quads` as a zero-copy `(Q, 4)` uint32 view, empty `(0, 4)` when
      absent; `Mesh.quad_count`; `Mesh.quad_report`.
- [x] 8.3 Docstrings carry the retopology disclaimer and the "approached, not
      hit" wording.
- [x] 8.4 `tools/check_binding_parity.py` passes with no new exemption — every
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
- [x] 9.7 The search lands within tolerance on a sphere and on a shape with
      thin features; the ceiling case reports `clamped`; an explicit cell size
      spends zero iterations.
- [x] 9.8 Export: OBJ/PLY/FBX carry four-corner faces; GLB carries triangles;
      a triangle mesh's bytes are unchanged in all four.
- [x] 9.9 Stream: quads survive a document round trip; a pre-quad reader's
      bytes still load; a corrupt tail is refused; a triangle mesh's bytes are
      unchanged.
- [x] 9.10 Decimation drops quads; transform keeps them; mixed concat drops
      them; a mesh layer round trip keeps them.
- [x] 9.11 asan/ubsan clean on the whole quad path, including the copy-out
      calls with a wrong buffer size.

## 10. Docs and gallery

- [x] 10.1 `examples/44_quad_export.py`: an SDF document and a voxel sculpt,
      OBJ + PLY + FBX + GLB, printing requested against actual, with the
      retopology disclaimer in the header comment.
- [x] 10.2 `examples/run_all.py` and the gallery README pick it up.
- [x] 10.3 `docs/05-claycore-library.md` and `docs/08-mesh-readback.md` describe
      the quad path, the count contract and the glTF exclusion.
- [x] 10.4 `docs/RELEASE.md`: the new ABI surface, the appended stream section
      that older readers skip, and — first, not buried — that this is a lattice
      quad grid rather than retopology.

## 11. Review round 1

- [x] 11.1 `mesh::fit_quad_ladder` — the discrete stack walk the faces header
      always described: coarsest level first, stop at the first level that
      reaches the target, keep the nearer of the two it brackets. Faces mode
      uses it instead of running the continuous secant and rounding each step
      to the nearest level, which walked no order, never tested for overshoot,
      and left `clamped` meaning something the header did not say.
- [x] 11.2 `clamped` in faces mode means the STACK ran out, and the walk cannot
      be stopped short by `max_iterations`; grid.h, clay.h, the pyclay
      docstring and the spec all say the same thing.
- [x] 11.3 The per-level memoisation map goes with it: the walk holds the best
      mesh and the current one, not every level it meshed.
- [x] 11.4 pyclay refuses a non-finite `cell_size` where the C ABI already did,
      instead of letting a NaN fall through to "no cell size given".
- [x] 11.5 `mesh/quad_mesh.h` states two more properties of the output: the
      near-degenerate quads a thin symmetric feature produces (inherited from
      the nets triangles, not introduced here), and diagonal occupancy as the
      second, more common cause of non-manifoldness.
- [x] 11.6 The count is documented as non-monotonic in the TARGET as well as in
      the cell size, because a host wiring a slider will see it move backwards.
- [x] 11.7 `examples/44_quad_export.py` asserts the dual/faces count identity it
      previously guarded behind an `if`, matching what the gallery README
      already states.

## 12. Review round 2

- [x] 12.1 **REGRESSION** (`the ladder charges every level it passes, not the
      bracketing pair`): the ladder walk's cost was documented as "two meshes
      whenever the target falls inside the stack" in `quad_mesh.h`, `grid.h`
      and the voxel-engine spec. It is not — the walk always starts at rung 0
      and charges every rung on the way, so a target met at level k costs k+1.
      Measured on a five-level grid (per-level 54/216/864/3456/13824): target
      400 -> 3 iterations, 1500 -> 4, 5000 -> 5, all with `clamped` false. The
      branch's own test already asserted 3 for a bracketed case, so the header
      was refuted inside the same commit. All four documents now state k+1, and
      the new test pins it for every level of a four-level stack.
- [x] 12.2 **REGRESSION** (`an untargeted fit at a level the grid does not have
      is empty, in both modes`): `mesh_quads_fit` with no target in faces mode
      clamped an out-of-range level onto the finest and meshed it — 600 quads
      where `mesh_quads` with the same options returns 0 — breaking "with no
      target this is mesh_quads with a report attached" and disagreeing with
      dual mode. It now returns the empty mesh and the zeroed report.
- [x] 12.3 **REGRESSION** (`c abi: the count knobs default at zero and are
      bounded at the far end`, `test_the_quad_count_knobs_take_the_c_abis_rules`):
      the two bindings disagreed about the knobs' defaulting rule. `tolerance`
      and `max_iterations` of 0 now mean the default in BOTH — that is what the
      C descriptor documents and what a caller declaring only the original
      layout sends — a negative cap and a tolerance of 1 or more are refused in
      both, pyclay caps `target` at `CLAY_MAX_BATCH` as the C ABI does, and a
      target too large for a `long long` raises this API's error instead of
      nanobind's `std::bad_cast`. The pyclay test that asserted `tolerance=0.0`
      raises is updated deliberately, with the reason in place.
- [x] 12.4 The ladder's rising count is stated as an ASSUMPTION ABOUT THE
      SOURCE rather than a property of ladders, since it is what licenses the
      early stop; the note names what would break (a stop at the first rung
      reporting `clamped` while a nearer rung sat unmeshed) and why the voxel
      stack satisfies it despite not being a strict mip.
- [x] 12.5 `fit_quad_cell` split along its seams — `clamp_cell`, `within_count`,
      `step_ratio`, `searchable_range`, `mesh_once` — taking it from CCN 22 /
      60 NLOC to 12 / 34 and leaving `src/mesh/quad_mesh.cpp` with no lizard
      warning. `within_count` is a real dedup: the loop's stop condition and
      the reported `within_tolerance` were the same expression written twice,
      in both the cell search and the ladder walk.

## 13. Review round 4

- [x] 13.1 The fine limit of the count search is a cell size the mesher will
      actually mesh. It was bisected in double and narrowed with a plain cast
      to float; round-to-nearest lands below the bound about half the time, and
      a cell below the bound is one `mesh_tape_quads` refuses — so a target past
      the ceiling clamped onto a refused lattice, got an empty mesh, and
      reported `quad_count = 0` with `clamped` set. Through the C ABI that came
      back as `CLAY_ERROR_BACKEND` "quad meshing produced no faces": a
      resolution limit read as a shape that vanished. Measured on a sphere of
      radius 0.4: the floor priced at 268,435,462.24 samples against a ceiling
      of 268,435,456, so every target at or above 2,481,308 returned nothing
      where one float ulp coarser meshes 1,948,668 quads; 245 of 500 random
      boxes had the same floor.
- [x] 13.2 The narrowed floor is stepped back up until the pricing accepts it,
      and the bisection runs 64 halvings rather than 40 so the cast is the only
      rounding left — at 40 the bound itself was a float ulp or two wide once
      the floor sat several decades below the coarse end, which gave away
      resolution the mesher would have granted.
- [x] 13.3 `finest_affordable_cell` and `lattice_affordable` are named in
      `quad_mesh.h`, and `mesh_tape_quads` prices through the same
      `lattice_affordable` the floor is checked against — the defect was two
      spellings of one bound, so there is now only one.
- [x] 13.4 Regression test drives the REAL pricing floor rather than a
      caller-supplied `min_cell_size` (which is affordable by construction and
      never exercised the narrowing): the reported region plus a sweep of cubes
      and slabs, each floor affordable and each one step finer refused, the
      mesher's own refusal asserted on the side that costs nothing, and the
      end-to-end search driven over a mesher that refuses exactly what
      `mesh_tape_quads` refuses.
- [x] 13.5 The C header's blanket "the READERS still fan-triangulate" is
      replaced by the per-format statement `mesh_io.h` already carries, and the
      flipped-quad figure in both is corrected to what is measured: 43% to 50%
      of the quads over cell sizes 0.3 down to 0.06 on a sphere of radius 1,
      not 40% and not 80%.
