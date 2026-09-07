## 1. Audit before designing

- [x] 1.1 The crossing table, every mesh-layer entry point, in `design.md`
- [x] 1.2 REPRODUCED through the C ABI: a world raycast hit fed to a stamp on a
      layer translated by 3 and scaled by 2 moves 0 vertices and returns CLAY_OK
- [x] 1.3 Second defect found by the audit: `clay_mesh_frame` carries a UNIFORM
      scale because `math::Transform` is a similarity, while a layer also
      carries `scale_axes` and `scene::layer_matrix` composes both — so a
      non-uniformly scaled layer cannot express its own frame to the sculptor
- [x] 1.4 Correct the roadmap's framing: layer-local arrays are the right
      contract and are not the defect

## 2. The contract, in the header

- [x] 2.1 Every crossing states its space beside itself in `clay.h`
- [x] 2.2 `moved == 0` documents that a space mismatch is no longer one of the
      things it can mean

## 3. The session frame

- [x] 3.1 `clay_mesh_sculptor_set_world_frame`
- [x] 3.2 `clay_mesh_sculptor_use_layer_transform`, refused for a standalone mesh
- [x] 3.3 Absent frame == identity == today's behaviour, exactly
- [x] 3.4 The per-axis scale reaches the sculptor, or the call refuses
- [x] 3.5 `MeshSculptor` stays representation-neutral: the adaptation is at the
      document/ABI wrapper, not in the sculptor

## 4. Helpers, once

- [x] 4.1 One place for point, vector, normal and ray conversion
- [x] 4.2 Normals through the inverse transpose, never the position map

## 5. Tests

- [x] 5.1 An ASYMMETRIC mesh, so an axis or rotation error is obvious
- [x] 5.2 Translation, rotation, uniform scale, non-uniform scale, combined
- [x] 5.3 A transformed raycast feeding a transformed stamp lands where an
      untransformed copy does, and moves the same count
- [x] 5.4 Normals under a non-uniform scale — REFUSED rather than carried, so
      the gate is the refusal. Under a similarity a normal is carried by the
      rotation, which IS the inverse transpose, so no separate normal path
      exists to drift
- [ ] 5.5 Undo/redo and save/reopen — NOT NEEDED and recorded rather than
      ticked: the frame is SESSION state on the sculptor handle, not document
      state. Nothing about it is recorded, undone or serialized, and the
      layer transform it adopts already has its own undo and persistence
      gates
- [x] 5.6 Bounds, the combined export and the sculptor agree about where the
      layer is — for every frame the sculptor accepts; a per-axis scale is the
      one case where bounds and export go further than the brush can, and it
      is refused rather than approximated
- [x] 5.7 PROVEN TO CATCH ITS REGRESSION

## 6. Verification

- [x] 6.1 Full unit suite green
- [ ] 6.2 `python3 tools/release_check.py --skip-slow`
- [ ] 6.3 CI green
