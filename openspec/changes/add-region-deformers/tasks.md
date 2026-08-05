# Tasks: add-region-deformers

## 1. Kernel
- [x] 1.1 `cgrab_point(p, centre, radius, displacement, ease)` in `deform.h`, identity outside the radius
- [x] 1.2 `cpose_point(p, centre, radius, pivot, rotation, translation, ease)`
- [x] 1.3 Front-facing gate evaluated against the local gradient
- [x] 1.4 `cfi_grab` / `cfi_pose` exactness helpers alongside the existing ones

## 2. Tape and scene
- [x] 2.1 `cdeform_grab` / `cdeform_pose` opcodes; `Deformer::ext_count` covers their wide params
- [x] 2.2 Factories with a positive-radius guard
- [x] 2.3 Bounds: dilate by the displacement; pose by the rotated extent
- [x] 2.4 Exactness wired through `deformer_lipschitz`

## 3. Voxels
- [x] 3.1 `voxel_grab` resampling occupancy and palette index from the inverse-displaced cell
- [x] 3.2 Snapshot-before-write, as the other verbs do, so order does not matter

## 4. Bindings and verification
- [x] 4.1 Both bindings, including the voxel verb
- [x] 4.2 Tests: tape-vs-kernel, locality (nothing beyond the radius moves), falloff shapes the result, front-facing leaves the far side alone, bound containment, round trip, C-vs-scene
- [x] 4.3 Voxel-vs-SDF agreement to within the voxel size
- [x] 4.4 Parity scenes; docs; ABI 0.10.0; full verification
