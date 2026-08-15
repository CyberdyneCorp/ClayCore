# Tasks — one cage over a layer

## 1. Kernel

- [x] 1.1 `cdeform_lattice_xform`, blob-carried transform and inverse beside
      the offsets
- [x] 1.2 The axis-aligned opcode unchanged, on its own path
- [x] 1.3 Kernel dialect gate green on all five backends

## 2. Scene

- [x] 2.1 `Deformer::lattice_transformed(box, divisions, local_to_cage)`
- [x] 2.2 Transform and inverse emitted into the blob
- [x] 2.3 Bounds: hull grown by the largest offset over the scale; Lipschitz
      unchanged, with the similarity argument recorded where it is computed
- [x] 2.4 `.clayspace` codec carries the transform

## 3. The resolver

- [x] 3.1 `brush::lattice_gizmo(layer, cage)` returning one warp per item
- [x] 3.2 Groups take none; the warp belongs at the front of the chain
- [x] 3.3 An untouched cage resolves to nothing

## 4. Bindings

- [x] 4.1 C ABI
- [x] 4.2 `pyclay`
- [x] 4.3 Binding parity gate green

## 5. Evidence

- [x] 5.1 An identity transform equals the axis-aligned cage, pointwise
- [x] 5.2 A rotated item warps to the same WORLD field as an unrotated one at
      the same pose — the claim the transform exists for
- [x] 5.3 The bound is unchanged by rotation and uniform scale
- [x] 5.4 A cage over two items reaches both
- [x] 5.5 Every item is reached, including one outside the box
- [x] 5.6 An untouched cage resolves to nothing
- [x] 5.7 `check_conservative_steps` over a transformed cage
- [x] 5.8 Round trip through the `.clayspace` codec
- [x] 5.9 An example with a committed render, inspected
- [x] 5.10 The axis-aligned path is a SEPARATE opcode, so it is unchanged by
      construction rather than by measurement — and it is dispatched FIRST, so
      the transformed form does not even cost it a compare

## 6. Docs

- [x] 6.1 `docs/07`: the gizmo row gains the resolver
