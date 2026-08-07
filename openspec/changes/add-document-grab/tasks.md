# Tasks: add-document-grab

- [x] 1.1 `SetDeformersCmd`: set an existing item's deformer chain, with the
      previous chain as its inverse. New tag, `kSceneMinor` bump, round trip
- [x] 1.2 `brush::grab_local`: map a world drag into one item's local frame —
      the pure, testable core of the whole row
- [x] 1.3 `brush::grab_document`: enumerate the items a world drag reaches and
      return the plan, skipping hidden and protected layers
- [x] 1.4 Cull by influence bound, so a local gesture does not cost the whole
      document its step scale
- [x] 1.5 Coalesce a trailing grab of the same centre and radius rather than
      appending, so a drag does not grow the chain without bound
- [x] 1.6 Python and C ABI bindings
- [x] 1.7 Tests: it drags a multi-item form as one surface, it equals a
      single-item grab where the form IS one item, items out of reach are
      untouched, a drag coalesces, the command round-trips and inverts, and a
      mirrored item is grabbed symmetrically
- [x] 1.8 Docs, example, full verification

Found while building:

- [x] 1.9 There is no command that changes an existing item's deformer chain.
      The vocabulary carries deformers only inside a whole `Node` in
      `AddNodeCmd`, so without `SetDeformersCmd` the resolver would have had no
      undoable way to apply its plan and a host would remove and re-add every
      node on every frame of a drag. The scene-model spec already required that
      commands carry a deformer chain; this completes it for editing.
- [x] 1.10 Factored the deformer encoding out of `write_node`/`read_node` into
      `write_deformers`/`read_deformers` so the new command reuses the SAME
      bytes rather than a second copy that could drift.
- [x] 1.11 The mapping is exact, and measured so: the same world surface placed
      on the node or on the layer comes back byte-identical after the same drag
      (0.00e+00 over 6000 probes), and ten drag frames equal one drag of the
      same distance to 0.00e+00 with an identical step scale.
- [x] 1.12 Coalescing is tested at the BOUNDARY as "ten frames equal one drag"
      rather than by counting deformers. Counting is an internal detail the C
      ABI cannot see; the field identity is the property that actually matters
      and it catches an appending bug just as surely.
- [x] 1.13 The C ABI deliberately does NOT expose setting an arbitrary deformer
      chain, and the spec was amended to say so. Nothing across the boundary
      needs it — deformers are built on an item before it is added, and the one
      edit that changes a chain on an existing item is this drag.
- [x] 1.14 The first render showed nothing: three balls at r=0.42 blended with
      k=0.22 read as one egg, so all three tiles looked identical while the
      caption claimed one of them pulled a ball out of the form. Articulating
      the form (r=0.38, k=0.12) and moving the camera side-on made the
      difference visible — the one-item drag bulges lopsidedly, the resolver
      arches the whole form.
- [x] 1.15 The directions sheet had the same problem in reverse: viewed side-on,
      a drag TOWARD the camera is invisible, so the `front_only` pair rendered
      identically while the caption promised a difference. Shot from above
      instead, where +Z reads.
