# Tasks: add-sculpt-handoff-export

## 1. Read their spec rather than guessing at it

- [x] 1.1 The format is `CyberRemesherAndUV/docs/sculpt-handoff-format.md`,
      version 1.0, and that repository ships the READING half only. It says the
      agreement with ClayCore is outstanding and that no negotiation has taken
      place — so this is the other half, not a re-specification
- [x] 1.2 Their CLI already assumes ours exists:
      `producer --for-retopo | cyberremesh --target -`
- [x] 1.3 MEASURED against our tree, not assumed. Missing: the two comment
      lines, and `material_mix`. Present: positions, indices, colours, and
      normals WHEN the mesh has them
- [x] 1.4 TWO HAZARDS their reader enforces and our writer would have violated,
      neither of them in the task list before reading the reader:
      `save_ply` declares a mesh's QUADS as its faces, and their reader rejects
      any face that is not a triangle — so our best export is exactly the one
      they would refuse. And normals are required, while a marching-cubes mesh
      has them only if gradients were requested

## 2. Decide

- [x] 2.1 DECIDED: `material_mix` comes from a MASK, and ClayCore does not gain
      material slots. A mask is already a painted scalar in [0,1] resolvable at
      any point — the shape and the meaning their channel asks for. No mask
      means zeros, which is the honest answer for a document that never
      expressed one
- [x] 2.2 DECIDED: both profiles. The buffer profile is nearly free, since
      positions, normals, colours and indices are already borrowed pointers, and
      it is the one that matters on a tablet where both engines share a process
- [x] 2.3 DECIDED: do NOT duplicate their `BufferView` struct. They own it; we
      produce the one array they cannot get from our existing accessors

## 3. The writer

- [x] 3.1 `mesh::vertex_normals`, area-weighted, in the mesh module. The cross
      product's LENGTH is twice the triangle's area, so accumulating it
      unnormalised IS the weighting — no separate term and no extra square root
      per face. A vertex no triangle touches gets +Y rather than a zero vector:
      a zero normal is not a direction, and a consumer that normalises it gets
      NaN, which reaches their bake as a black texel rather than an error
- [x] 3.2 The PLY profile, ascii and binary
- [x] 3.3 Computed into a local, so writing never modifies the caller's mesh
- [x] 3.4 Resolved per vertex from the mask, clamped, or zeros

## 4. Prove it

- [x] 4.1 The scenarios in the spec delta, in C++ and in pyclay
- [x] 4.2 Covered twice: a unit test on the written bytes, and end to end
      against their actual CLI
- [x] 4.3 Both halves asserted
- [x] 4.4 DONE, and it is the strongest evidence here. Their CLI is built in
      the sibling checkout, so a ClayCore handoff went through their real
      reader rather than ours:

      | | |
      |---|---|
      | ClayCore sphere, 11 160 tris | accepted, retopologized to 1 268 quads |
      | ClayCore QUAD export, 1 830 quads | accepted, `droppedFaces: 0`, 708 quads out |
      | the same quad mesh via ordinary `save()` | **REJECTED** — "not a sculpt handoff" |

      Their report reads back `producer: claycore`, `hasMaterialMix: true`,
      `hasVertexNormals: true`, `hasVertexColors: true`. The third row is the
      point: the hazard was real, and a test written only from our side would
      have proved we agree with ourselves
- [x] 4.5 The quad fixtures assert `quad_count > 0` first, and the mask fixture
      covers the UPPER HALF so the column carries both values — a mask covering
      everything, or nothing, compares a column against itself

## 5. Reach it

- [x] 5.1 C ABI: `clay_mesh_save_handoff`, `_memory` for the pipe route their
      CLI documents, and `clay_mesh_handoff_material_mix`
- [x] 5.2 pyclay: `Mesh.save_handoff` and `Mesh.handoff_material_mix`
- [x] 5.3 Reachable from C and from pyclay, and exercised end to end through
      their CLI from a pyclay script
- [x] 5.4 `docs/08-mesh-readback.md`, including the field-evaluator table —
      see 6.2

## 6. Say what was assumed

- [x] 6.1 Recorded in `docs/RELEASE.md`: the handoff's version is THEIRS, their
      versioning policy governs it, and `CLAY_HANDOFF_VERSION_*` move only when
      their spec does. The one field that cost a decision rather than a line of
      code is `material_mix` being required of a producer with no material
      slots

- [x] 6.2 THE BAKE SEAM IS ALREADY HALF-BUILT ON THEIR SIDE, found by reading
      their `add-claycore-bridge` tasks rather than by asking. They have a
      `FieldEvaluator` interface, a `CyberFieldEvaluator` C struct of three
      callbacks, a `cyber_bake_field` entry point and a Python subclassable
      base — and no volumetric engine to plug into it: "this build links no
      volumetric engine, so the report's `fieldSampledMaps` is always []".

      ClayCore is that engine, and NOTHING FURTHER IS NEEDED FROM THIS ABI to
      fill their callbacks. The correspondence is documented in
      `docs/08-mesh-readback.md` and in `clay.h`:

        distance  -> clay_eval_points
        gradient  -> clay_eval_gradients
        occlusion -> 1.0f - clay_measure_points(CLAY_MEASURE_OCCLUSION)
        curvature -> leave it to their default

      TWO TRAPS IN THAT TABLE, both stated where a host will meet them. Their
      `occlusion` is OPENNESS (1 = fully open) and ours is occlusion (1 = fully
      enclosed) — passing ours straight through bakes an inverted AO map, which
      looks plausible and is wrong everywhere. And CLAY_MEASURE_CURVATURE is a
      saturated [0,1] masking value while theirs is signed mean curvature in
      1/length units where a sphere of radius r reads 1/r; they are not the
      same number
