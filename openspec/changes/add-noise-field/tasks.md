# Tasks: add-noise-field

- [x] 1.1 `cuint` in the shim — the dialect's first integer type
- [x] 1.2 `include/clay/kernel/noise.h`: integer-hashed gradient noise, fractal by octaves
- [x] 1.3 `cdeform_noise` contributing a distance offset, beside `displace`
- [x] 1.4 `cfi_noise`: the gradient summed over octaves
- [x] 1.5 Bounds: the surface can move by the amplitude, so the item's bound grows by it
- [x] 1.6 C ABI and Python bindings
- [x] 1.7 A parity corpus row, so all four backends are held to it
- [x] 1.8 Tests: it roughens, zero is a no-op, the same seed repeats, a different seed
      differs, it is not periodic, the declared Lipschitz holds, a ray still lands
- [x] 1.9 Docs, example, full verification

Found while building:

- [x] 1.10 The example's first attempt to show noise is not a sine compared the
      two fields one lattice spacing apart, which is a distance the sine is
      under no obligation to repeat at — and then, corrected to the sine's true
      period, compared the raw FIELDS, where the sphere's own distance changes
      far more over a period than either deformer does. Both readings were
      meaningless and the second made the two look identical. It now subtracts
      the undeformed sphere, which isolates the deformer exactly because both
      offset the distance after the primitive: the sine then repeats to 0.0000
      and the noise does not.
- [x] 1.11 Gradient noise is exactly zero on the integer lattice, by
      construction — every corner's contribution is a dot product with a zero
      offset. It looks like a bug the first time it is measured, so there is a
      test pinning it.
