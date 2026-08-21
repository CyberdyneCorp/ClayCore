# Tasks

## 1. Decide the shape before writing any of it

- [ ] 1.1 CONFIRM the forward maps against the kernel's inverse ones, in a
      test rather than by reading: for `taper` and `twist` the weight depends
      only on the axis coordinate and neither map moves it, so forward and
      inverse are exact mutual inverses. Assert that round-tripping a point
      through both returns it, or the premise of this change is wrong
- [ ] 1.2 DECIDE where the forward maps live. Two candidates: beside the
      inverse ones in `kernel/deform.h` (one file, five dialects recompiled,
      and the mesh path is CPU-only so four of them gain nothing), or in
      `mesh/` as CPU-side functions. Prefer the second unless a GPU mesh
      deform is foreseen — and say so either way
- [ ] 1.3 DECIDE the frame's spelling: reuse `clay_mesh_frame` (already the
      "where a mesh sits" descriptor, used by the raycast) or a deformer-owned
      origin+axis pair. Reuse unless the quaternion-plus-uniform-scale shape
      cannot express what a gizmo needs
- [ ] 1.4 DECIDE whether the ranged forms (`twist_range`, `bend_range`) are
      separate verbs or a span on the base verb. The SDF side has them
      separate; a mesh has no reason to inherit that if one span parameter
      covers both

## 2. The forward maps

- [ ] 2.1 `taper` — scale across a span along the frame axis, eased
- [ ] 2.2 `twist` — rotate about the axis proportionally to distance along it
- [ ] 2.3 `bend` — the same with a bend rather than a twist
- [ ] 2.4 The ranged behaviour: held beyond the span rather than continuing,
      which is what makes a gizmo box's ends mean something
- [ ] 2.5 Each map is exact at zero: a zero angle, a unit scale or an empty
      span moves NO vertex, bit-identically, so an untouched control is not a
      no-op that still rewrites every position

## 3. The walk

- [ ] 3.1 `MeshSculptor::apply_deformer`, modelled on `apply_lattice`: weld
      classes, not raw vertices
- [ ] 3.2 The mask scales the warp — a fully masked class stays bit-identical
      to its rest position, by the rule every other verb follows
- [ ] 3.3 `VertexDeltas` records it, so a deform reverts and re-applies
      bit-identically like every other gesture
- [ ] 3.4 Normals recomputed over the touched classes and their rings, and the
      deferred-normals path honoured as the brushes do
- [ ] 3.5 Skip the walk entirely when the deformer is an identity, so an
      untouched gizmo costs nothing and leaves no undo entries

## 4. Pin the properties

- [ ] 4.1 Topology byte-identical: `indices` and `quads` unchanged
- [ ] 4.2 A seam does not crack — a mesh with split vertices at a hard edge
      comes out with those vertices still coincident, bit for bit
- [ ] 4.3 A fully masked region is bit-identical to its rest position
- [ ] 4.4 Deterministic: same mesh, same frame, same parameters, same bytes
- [ ] 4.5 Reverts and re-applies bit-identically through one record
- [ ] 4.6 An identity deformer moves nothing and records nothing
- [ ] 4.7 AGREEMENT WITH THE SDF SIDE, which is the test worth having: taper
      and twist the same shape as a mesh layer and as an SDF item, mesh both,
      and require them to agree to within the sampling tolerance. If they do
      not, one of the two maps is wrong and this says which

## 5. Reach it from both bindings

- [ ] 5.1 C ABI entry points on the mesh sculptor
- [ ] 5.2 pyclay, matching so `check_binding_parity` stays clean
- [ ] 5.3 ABI 0.38.0 in all three places the release checklist names

## 6. Say what it is and is not

- [ ] 6.1 Spec delta on `meshing`
- [ ] 6.2 `docs/sculpt_comparison.md`: the Deformation-palette row moves from
      "SDF items only" to what it becomes here, and keeps saying what a voxel
      layer still cannot do
- [ ] 6.3 `docs/07-brushes-and-features.md`: deformers are not brushes and
      belong in their own section, with the frame convention stated once
- [ ] 6.4 An example showing a taper and a twist on a mesh layer, and the
      Relax pass that recovers the stretched triangles — the pairing is the
      point, since there is no remesher and the docs should not imply one
