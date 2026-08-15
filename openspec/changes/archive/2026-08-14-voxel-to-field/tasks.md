# Tasks: the sculpt comes back

## 1. The conversion

- [x] 1.1 `VoxelGrid::to_field(level, options)` over `FieldVolume::sample`.
- [x] 1.2 Occupancy read by trilinear interpolation between cell centres, so
      the isosurface is smooth rather than a staircase.
- [x] 1.3 `field::redistance` on the result, so it is a distance field.
- [x] 1.4 A single palette index converts alone, which is how colour survives.
- [x] 1.5 The occupancy reader shared with the smooth mesher rather than
      duplicated.

## 2. The C ABI

- [x] 2.1 `clay_voxel_to_layer`: a new layer, one volume item per palette
      entry, each with its colour.
- [x] 2.2 Nothing is modified when the conversion cannot produce anything.

## 3. Tests

- [x] 3.1 The acceptance test: block out with booleans, convert, sculpt with
      the voxel verbs, convert back, boolean again.
- [x] 3.2 The surface comes back within about a cell, sampled off axis as well
      as on.
- [x] 3.3 The field measures distance rather than occupancy.
- [x] 3.4 One palette entry converts alone; an absent entry converts to
      nothing.
- [x] 3.5 The grid is unchanged by converting.
- [x] 3.6 Through the C ABI: the layer holds one item per entry with its
      colour, it evaluates solid, and an empty grid leaves no layer behind.

## 4. Still open, deliberately

- [x] 4.1 A colour channel on `FieldVolume`, which makes this one item rather
      than one per entry. Deferred here deliberately and then delivered by the
      `volume-color-channel` change (PR #115): a sampled volume carries an
      optional packed-RGB8 colour per sample, so `clay_voxel_to_layer` now
      produces ONE item carrying the palette.
- [x] 4.2 pyclay surface (`Volume.from_voxels`) and a gallery example
      (`42_representation_round_trip`) showing the round trip.
- [x] 4.3 The SDF-to-voxel direction's guarantees written down, which the issue
      asks for and which is prose rather than code.
