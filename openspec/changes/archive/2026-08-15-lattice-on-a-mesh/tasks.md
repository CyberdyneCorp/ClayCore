# Tasks — a lattice on a mesh layer

## 1. The cage

- [x] 1.1 `mesh::Lattice` over a box, free resolution per axis (>= 2),
      storing control-point OFFSETS
- [x] 1.2 Trivariate Bernstein evaluation, one formula for every size
- [x] 1.3 Parameters clamped, so material outside the box travels rigidly
- [x] 1.4 `is_identity` — an untouched cage is exactly the identity

## 2. The verb

- [x] 2.1 `MeshSculptor::apply_lattice`, recording into `VertexDeltas`
- [x] 2.2 Normals recomputed for what moved
- [x] 2.3 Topology untouched — the contract every mesh verb holds

## 3. Bindings

- [x] 3.1 C ABI: the cage plus apply
- [x] 3.2 `pyclay` `Lattice` and `MeshSculptor.lattice(...)`
- [x] 3.3 Refusals: a resolution below two, a degenerate box
- [x] 3.4 Binding parity gate green

## 4. Evidence

- [x] 4.1 An untouched cage is bit-identical, positions and normals
- [x] 4.2 A uniform offset translates the mesh exactly
- [x] 4.3 A 2x2x2 cage matches an independently computed trilinear blend
- [x] 4.4 Material outside the box travels rigidly rather than collapsing
- [x] 4.5 Topology byte-identical, triangles and quads
- [x] 4.6 One undo step: apply then revert is bit-identical
- [x] 4.7 Corners interpolate — dragging a corner moves that corner exactly
- [x] 4.8 Refusal cases
- [x] 4.9 An example with a committed render, inspected

## 5. Docs

- [x] 5.1 `docs/07` ZBrush-equivalence row: available on mesh layers, still
      absent on SDF items, with the reason
- [x] 5.2 README / survey docs where the mesh verbs are counted
