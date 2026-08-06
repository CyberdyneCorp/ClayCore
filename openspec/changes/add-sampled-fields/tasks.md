# Tasks: add-sampled-fields

- [x] 1.1 `FieldVolume`: sparse narrow-band bricks with a halo, built from a callable
- [x] 1.2 Dense brick index with inside/outside sentinels for bricks off the band
- [x] 1.3 `ctape_volume` opcode with trilinear interpolation; blob layout
- [x] 1.4 `PrimType::Volume`; items carry a volume by shared reference
- [x] 1.5 Exactness: inexact, and conservative outside the band
- [x] 1.6 Bounds; serialization as a document chunk
- [x] 1.7 Python bindings; parity corpus row so all four backends are verified
- [x] 1.8 Tests: reproduces its source, sparse storage, sign far from the band, combines, inexact, conservative bound, round trip, empty volume
- [x] 1.9 Docs, example, full verification

Found while building, and fixed here rather than deferred:

- [x] 1.10 An empty brick reported a flat band width, so a marcher crossing the
      empty majority of the region took steps that never grew and ran out of
      iterations. Each empty brick now carries its Chebyshev gap in bricks to
      the nearest brick that has samples.
- [x] 1.11 Outside the sampled box the value was the distance to the BOX, which
      falls to zero on the box face — every ray hit an invisible shell where
      the sampling stopped. Folded together by Pythagoras with the field at the
      projected point, which is exact for a projection onto a convex set.
- [x] 1.12 The declared Lipschitz was 1. Trilinear interpolation of a
      1-Lipschitz field can reach sqrt(3); see cfi_volume.
- [x] 1.13 CLAY_PRIM_VOLUME was constructible through the C ABI despite nothing
      being able to give it samples, so it built a silently empty item.
      Refused until mesh import gives it a source.
