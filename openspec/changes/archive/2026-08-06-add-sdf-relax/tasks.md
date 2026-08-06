# Tasks: add-sdf-relax

- [x] 1.1 `field::relax`: smooth a sampled volume into a new one
- [x] 1.2 A spherical region with a falloff, so it is a brush and not only a filter
- [x] 1.3 Iterations
- [x] 1.4 Python bindings, and a C ABI entry point so an imported scan can be smoothed
- [x] 1.5 Tests: a bumpy surface gets smoother, a smooth one barely moves, the
      region is respected, the Lipschitz bound survives, sphere tracing still
      cannot overstep, and volume is not gained without limit
- [x] 1.6 Docs, example, full verification

Found while building, and fixed here rather than deferred:

- [x] 1.7 The first implementation smoothed by re-sampling through `eval()`,
      which was wrong in a way that only showed up as a number: a volume's
      value where it has no samples is a flat BOUND, not a distance, and
      re-sampling a field that mixes the two bakes the boundary between them
      into adjacent samples one cell apart — turning a brick-face artifact into
      a genuinely steep interpolant. Measured slope went from 1.1 to 9.3, on an
      operation that provably cannot raise it. Relax now rewrites the stored
      samples in place.
- [x] 1.8 `FieldVolume::sample_at` looked up a coordinate on a brick face by
      applying one "prefer the lower brick" decision to all three axes at once,
      so a sample on two faces landed on the diagonal neighbour and read as
      missing. Each axis decides independently now, and up to eight bricks are
      tried.
- [x] 1.9 Relax MOVES the surface, and the sample-free bricks were classified
      against where it used to be, so their bounds would have overstated the
      distance to the surface that is now there. The band is narrowed by how
      far a pass can move it.
