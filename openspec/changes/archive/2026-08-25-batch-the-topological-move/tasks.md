# Tasks: move a document's material through the pool

## 1. Measure first

- [x] 1.1 #275 left `solve()` as an open question. Measured: 4-5% of the
      operation, 87k of 2.09M source calls. Batching its walk was never worth
      doing and the issue's guess that it might be is answered.
- [x] 1.2 The sampling pass is the operation, which is what the fix targets.

## 2. The source type

- [x] 2.1 `field::PointBatch` — packed xyz in, distances out, the same shape as
      `FieldVolume::ColorBlockFill`. NOT a `BrickBlockFill`, and the header
      says why: the query positions are the pulled-back points, so a fill that
      only knows the grid cannot answer them.
- [x] 2.2 `eval::tape_point_batch(tape)` beside `tape_block_fill`, with the
      same fallback to the tape's own scalar walk when no CPU backend is
      registered, and the same borrowing rule.

## 3. Sharing the graph

- [x] 3.1 `solve` splits into `make_grid` (the lattice, sized to the reach) and
      `solve_over` (the walk, over a material array it is handed). The only
      place the walk asks the source anything is that array.
- [x] 3.2 `solve` and `solve_batched` fill it a point at a time and a batch at
      a time respectively, over the same grid and the same walk.
- [x] 3.3 `pull_back` becomes one function both overloads call, so the
      displacement map cannot drift between them.

## 4. Tests

- [x] 4.1 Byte-identity over THREE paths, not one: a drag that moves material;
      a drag with zero displacement, which returns the source unchanged through
      a different function in each overload; and an anchor with no material
      within reach, which gives up before the walk and so exercises the
      make_grid / solve_over split.
- [x] 4.2 Full unit suite 1417, no failures.
- [x] 4.3 `examples/31_move_topological.py` exits 0 — the pyclay side driven
      end to end, since that is the only binding this verb's document form has.

## 5. The gate

- [x] 5.1 `BM_VolumeMoveDoc` and `BM_VolumeMoveSerialDoc`, the second kept as
      the reference the way the other two pairs keep theirs.
- [x] 5.2 Added to `FASTER_THAN`. Measured 25.7 ms against 412 ms.

## 6. Not in this change

- [ ] 6.1 A C ABI entry point. There is none for the document-sourced move and
      this change does not add one; the ABI's move takes an existing volume.
- [ ] 6.2 Batching the geodesic walk. Sequential by construction, 4% of the
      calls.
