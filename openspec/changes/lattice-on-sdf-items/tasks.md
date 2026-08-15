# Tasks — a lattice on SDF items

## 1. Kernel

- [x] 1.1 `clattice_point` in `deform.h`, trivariate Bernstein over blob-carried
      offsets, parameters clamped
- [x] 1.2 `cdeform_lattice` in the tape's deformer dispatch, reading the cage's
      blob offset, divisions and box from the record
- [x] 1.3 `cfi_lattice` in `exactness.h`: the Bernstein DERIVATIVE bound
- [x] 1.4 Kernel dialect gate green on all five backends

## 2. Scene

- [x] 2.1 `Deformer::lattice(box, nx, ny, nz)` carrying the offsets
- [x] 2.2 Offsets emitted into the blob at compile time
- [x] 2.3 Bounds: the item's bound grown by the largest offset; Lipschitz via
      `cfi_lattice`; the item marked inexact
- [x] 2.4 `.clayspace` codec writes and reads the cage — old files unaffected

## 3. Bindings

- [x] 3.1 C ABI entry point carrying the cage
- [x] 3.2 `pyclay` `.lattice(...)`
- [x] 3.3 Refusals: divisions out of range, a degenerate box
- [x] 3.4 Binding parity gate green

## 4. Evidence

- [x] 4.1 An untouched cage is the undeformed field, pointwise
- [x] 4.2 A uniform drag translates the field exactly
- [x] 4.3 The bound follows the DIFFERENCES, not the magnitudes
- [x] 4.4 `check_conservative_steps` over a non-uniform cage
- [x] 4.5 Material outside the box travels rigidly
- [x] 4.6 Two per axis is exactly trilinear; corners interpolate
- [x] 4.7 Refusal cases
- [x] 4.8 Parity-corpus scene with a NON-UNIFORM cage
- [x] 4.9 Round trip through the `.clayspace` codec
- [x] 4.10 An example with a committed render, inspected — including the
      travels-less-than-nominal character, measured rather than asserted

## 5. Docs

- [x] 5.1 `docs/07`: the Gizmo Lattice row now covers both forms, with the
      approximation stated for the SDF one
- [x] 5.2 Deformer counts recounted from source
