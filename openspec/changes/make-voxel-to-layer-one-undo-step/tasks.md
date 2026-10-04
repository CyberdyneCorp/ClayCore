# Tasks: make-voxel-to-layer-one-undo-step

## 1. Regression test

- [x] 1.1 `test_voxel_to_field.cpp` "a conversion into a layer is one undo
      step, and undoes completely": through the C ABI, undo depth grows by
      exactly 1, one undo leaves 0 layers, one redo restores the layer with
      its single volume item
- [x] 1.2 Prove it fails on unfixed `main`: depth grew by 2 and one undo left
      1 layer

## 2. Fix

- [x] 2.1 `clay_voxel_to_layer` inserts the volume node into the new layer's
      content before the layer is added, and adds it with one `AddLayerCmd`

## 3. Documentation

- [x] 3.1 `clay.h`: describe the single palette-carrying volume and the one
      undo step; drop "this in a loop" from `clay_item_volume_from_voxels`
- [x] 3.2 `c-abi` spec: MODIFIED requirement, every existing scenario kept,
      one scenario added for the undo step

ABI: unchanged (no entry point, no signature change).
