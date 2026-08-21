# Tasks

## 1. Decide the shape before writing any of it

- [x] 1.1 CONFIRMED, and it removed a deformer from the change — which is what
      a precondition is for. Round-trip error through forward-then-inverse:
      taper 1.2e-07, twist 2.4e-07, twist_range 2.4e-07 — **exact**. `bend`
      1.73 and `bend_range` 2.45 — **not inverses at all**. Bend takes its
      angle from `p.x` and then moves `p.x`; worse, it is not injective (at
      k = 0.9 rest points x = -1.74 and x = +1.75 land 0.0101 apart), so past a
      gentle angle no forward map exists. Fixed-point inversion converges at
      k = 0.3 and diverges from k = 0.9, exactly where the fold starts. Bend is
      out; the numbers are in the proposal and the roadmap
- [x] 1.2 DECIDED: `mesh/deform.h`, not the kernel header. A mesh deform runs
      once per vertex on the host, so putting the forward maps in a header
      compiled as five dialects would recompile four backends for a path none
      of them reaches. Revisit only if a GPU mesh deform is ever wanted.
      ORIGINAL WORDING: DECIDE where the forward maps live. Two candidates: beside the
      inverse ones in `kernel/deform.h` (one file, five dialects recompiled,
      and the mesh path is CPU-only so four of them gain nothing), or in
      `mesh/` as CPU-side functions. Prefer the second unless a GPU mesh
      deform is foreseen — and say so either way
- [x] 1.3 DECIDED: the deformer carries its own `origin` + `axis` + `span`
      rather than reusing `clay_mesh_frame`. That struct is a placement
      (position, quaternion, uniform scale) and a gizmo needs a span along an
      axis, which it cannot express without the caller composing a rotation by
      hand. ORIGINAL WORDING: DECIDE the frame's spelling: reuse `clay_mesh_frame` (already the
      "where a mesh sits" descriptor, used by the raycast) or a deformer-owned
      origin+axis pair. Reuse unless the quaternion-plus-uniform-scale shape
      cannot express what a gizmo needs
- [x] 1.4 DECIDED: one span parameter, not separate verbs. The SDF side has
      `twist` and `twist_range` as different deformers because an unranged
      twist acts on the whole item; a mesh deformer always has a frame and
      therefore always has a span, so the unranged form has nothing to mean.
      ORIGINAL WORDING: DECIDE whether the ranged forms are separate verbs. The SDF side has them
      separate; a mesh has no reason to inherit that if one span parameter
      covers both

## 2. The forward maps

- [x] 2.1 `taper` — scale across a span along the frame axis, eased
- [x] 2.2 `twist` — rotate about the axis proportionally to distance along it
- [x] 2.3 `bend` — REMOVED by 1.1, not implemented. See the proposal
- [x] 2.4 The ranged behaviour: held beyond the span rather than continuing,
      which is what makes a gizmo box's ends mean something
- [x] 2.5 Each map is exact at zero: a zero angle, a unit scale or an empty
      span moves NO vertex, bit-identically, so an untouched control is not a
      no-op that still rewrites every position

## 3. The walk

- [x] 3.1 `MeshSculptor::apply_deformer`, modelled on `apply_lattice`: weld
      classes, not raw vertices
- [x] 3.2 The mask scales the warp — a fully masked class stays bit-identical
      to its rest position, by the rule every other verb follows
- [x] 3.3 `VertexDeltas` records it, so a deform reverts and re-applies
      bit-identically like every other gesture
- [x] 3.4 Normals recomputed over the touched classes and their rings, and the
      deferred-normals path honoured as the brushes do
- [x] 3.5 Skip the walk entirely when the deformer is an identity, so an
      untouched gizmo costs nothing and leaves no undo entries

## 4. Pin the properties

- [x] 4.1 Topology byte-identical: `indices` and `quads` unchanged
- [x] 4.2 A seam does not crack — a mesh with split vertices at a hard edge
      comes out with those vertices still coincident, bit for bit
- [x] 4.3 A fully masked region is bit-identical to its rest position
- [x] 4.4 Deterministic: same mesh, same frame, same parameters, same bytes
- [x] 4.5 Reverts and re-applies bit-identically through one record
- [x] 4.6 An identity deformer moves nothing and records nothing
- [x] 4.7 AGREEMENT WITH THE SDF SIDE. Implemented as the sharper form: a
      point pushed through the mesh's forward map and back through the
      KERNEL's inverse map must return to where it started, which tests the
      maps directly instead of through two meshing tolerances. It earned its
      keep immediately — it failed at 1.36 on the first run and caught a
      LEFT-HANDED frame basis, which reversed every twist. The taper tests all
      passed while that bug was live, because a taper is rotationally
      symmetric about its axis and cannot see handedness

## 5. Reach it from both bindings

- [x] 5.1 C ABI entry points on the mesh sculptor
- [x] 5.2 pyclay, matching so `check_binding_parity` stays clean
- [x] 5.3 ABI 0.38.0 in all three places the release checklist names

## 6. Say what it is and is not

- [x] 6.1 Spec delta on `meshing`
- [x] 6.2 `docs/sculpt_comparison.md`: the Deformation-palette row moves from
      "SDF items only" to what it becomes here, and keeps saying what a voxel
      layer still cannot do
- [x] 6.3 `docs/07-brushes-and-features.md`: deformers are not brushes and
      belong in their own section, with the frame convention stated once
- [x] 6.4 `examples/57_mesh_deformers.py` — and it corrected the premise it was
      written on. The plan said to show "the Relax pass that recovers the
      stretched triangles". MEASURED, Relax does not recover a deformation:
      after a taper to 0.18 six relax passes move edge-length variation from
      0.2929 to 0.3050, slightly WORSE. The reason is in the example: a taper
      leaves the top ring with the same vertex count around a smaller
      circumference, so the damage is ANISOTROPY rather than uneven spacing,
      and a verb that slides vertices cannot change how many a ring has. Relax
      is the recovery for a large `grab`. There is none for this short of
      re-tessellation, and the docs now say so rather than implying otherwise
