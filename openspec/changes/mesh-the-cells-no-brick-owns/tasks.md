# Tasks: mesh the cells no surface brick owns

## 1. Establish that it is a defect

- [x] 1.1 The reported shape in-engine — a ball, forty relief dabs, every third
      incised: the brick mesh reports 28 open boundary edges at voxel 0.05 and
      386 at 0.02, and `mesh_tape` of the same document reports 0 at both.
- [x] 1.2 The whole-cache call and the all-surface-keys call give the SAME
      count, so it is not the subset path and not a host's per-key store.
- [x] 1.3 Counted directly: 236 crossing cells owned by non-surface bricks at
      voxel 0.02, which nothing marches. That is the hole, not a symptom.
- [x] 1.4 A hand-fed lattice to isolate steepness from everything else — a
      sphere field multiplied by L. Watertight through L = 4, open at L = 6,
      which is `sqrt(3)` voxels of cell diagonal against the 3-voxel band.
- [x] 1.5 That an ordinary document does NOT trigger it: strokes and hard
      unions on a sphere reported 0 crossing cells owned by non-surface bricks.
      The defect needs a field steeper than the band, which is why it survived.

## 2. The fix

- [x] 2.1 `shell_cells` stops consulting the owner's state. The filter existed
      to mirror the whole-surface path, and the whole-surface path was wrong.
- [x] 2.2 `collect_straddlers` runs on BOTH paths, not only on a subset.
- [x] 2.3 `cell_attribution` — the lowest requested key whose closed box holds
      one of the cell's eight corner lattice points. A cell owned by a brick
      with no lattice is kept whole under it, because its crossing vertices sit
      in the owner's box and the per-corner rule would drop all of them.
- [x] 2.4 The per-corner straddler rule is unchanged for cells whose owner IS a
      surface brick: those are marched by whoever requests that brick, so a
      subset still takes only what reaches into it.

## 3. Cost

- [x] 3.1 `shell_cells` walks owner bricks rather than each key's ring: each
      cell emitted once, in order, with no hash set and no sort. 42 ms -> 14 ms
      on a 6,003-brick surface.
- [x] 3.2 The cell march fans out across the pool, recorded and replayed in
      emission order — the same split, and the same reason, as the brick march.
      Byte-identical to marching them one at a time.
- [x] 3.2a `shell_cells` splits into `ring_owners`, `append_ring_cells` and
      `reaches_request`: the one function was five loops deep and clang-tidy
      would not even score it. Worst in the file is unchanged at 24
      (`apply_brick_attributes`, pre-existing); the new helpers score 21, 10
      and 5, inside the systems target.
- [x] 3.3 Whole path 171 -> 190 ms; 128-key subset 7.9 -> 5.9 ms. The frame
      path is FASTER than before, because it already paid the per-cell ring
      scan and no longer does.
- [x] 3.4 `check_bench.py`: no gate moves. Its 276-brick scene puts the
      difference inside the spread — Whole 7.0 -> 7.6 ms, Subset 0.7 -> 0.6 ms
      — and the ratio gate holds. Its one failure, `BM_MetalTapeResident` in a
      build with no Metal backend, fails identically on `main`.

## 4. Tests

- [x] 4.1 `tests/unit/test_mesh.cpp` — the reported shape at voxel 0.05,
      asserting watertight, manifold and oriented on the whole-cache mesh and
      on the all-keys mesh.
- [x] 4.2 VERIFIED to fail without the fix: 6 assertions, 28 boundary edges on
      both paths.
- [x] 4.3 Full unit suite: 1423 cases, 13,072,281 assertions, no failures.

## 5. Documentation

- [x] 5.1 `mesh_bricks` in `marching.h` states that a whole-surface mesh owes
      straddlers too, and why a cell owned by a lattice-less brick can cross.
- [x] 5.2 `clay.h` says the same at `clay_brick_cache_mesh`, where a host
      reads it.
- [x] 5.3 The meshing spec's "it marches every surface cell already, so it has
      no boundary and owes no straddlers" is replaced by what is actually true.

## 6. Not in this change

- [ ] 6.1 Classifying a brick from its neighbours' samples. `submit` sees one
      brick and cannot; the mesher is where both sides are visible.
- [ ] 6.2 Bounding what a document's Lipschitz may be. It is declared already.
