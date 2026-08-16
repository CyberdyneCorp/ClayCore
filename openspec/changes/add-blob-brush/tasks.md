# Tasks — Blob

- [x] 1.1 `cblob_offset` in `deform.h`, reusing `cnoise_fbm` and `cregion_weight`
- [x] 1.2 `cdeform_blob` in the distance-offset dispatch, not the point-warp one
- [x] 1.3 `cfi_blob`: the noise bound plus the region's own gradient
- [x] 1.4 Kernel dialect gate green on all five backends
- [x] 2.1 `Deformer::blob(...)`; hull grows by the AMPLITUDE, not the centre
- [x] 3.1 C ABI `CLAY_DEFORM_BLOB` with a radius refusal
- [x] 3.2 `pyclay` `.blob(...)`
- [x] 3.3 Binding parity gate green
- [x] 4.1 Untouched past the radius, at float equality
- [x] 4.2 One dab both swells and eats in
- [x] 4.3 A tighter radius costs more step scale — the region term proved
- [x] 4.4 The hull grows by the amplitude, not the centre
- [x] 4.5 `check_conservative_steps` over a blob
- [x] 4.6 Round trip through the `.clayspace` codec
- [x] 4.7 Parity-corpus scene with a non-trivial falloff AND fractal
- [x] 4.8 `examples/03_deformers.py` tile, and the finite-support gate extended
      to cover blob alongside grab, pose and magnify
- [x] 5.1 `docs/07` and `sculpt_comparison` rows; deformer counts recounted
