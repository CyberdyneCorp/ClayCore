# Tasks: a relax dab should cost what it moves

## 1. The traversal

- [x] 1.1 `FieldVolume::rewrite_region(region, fn)` walks the brick index range
      the region covers rather than every slot. Bounds from `floor`, rounded
      outward by one brick on each side and clamped to the grid.
- [x] 1.2 The precondition — `fn` is the identity outside `region` — is stated
      in the header WITH its two reasons, not as an assertion the reader has to
      take on trust. It cannot be checked at runtime and a caller who breaks it
      gets a wrong answer, so the comment is the only thing standing there.
- [x] 1.3 A region meeting no brick, and a region covering all of them, both
      behave: the first writes nothing, the second is `rewrite`.

## 2. Relax

- [x] 2.1 The region is `region_radius + tuned.falloff`, from the TUNED
      settings. Relax widens a falloff narrower than its kernel, so building
      the region from the caller's value would under-cover the taper — the one
      way to get this wrong that the conservative rounding would not absorb.
- [x] 2.2 `region_radius == 0` means everywhere, which is a filter rather than
      a brush, and takes the full sweep.
- [x] 2.3 No margin for the stencil. The taps come from `previous`, a whole
      unwritten copy, so a sample inside the region reads neighbours outside it
      freely; the region bounds where values CHANGE, and that set does not grow
      across passes. Written down in the code because the opposite looks
      obvious.

## 3. Tests

- [x] 3.1 `rewrite_region equals rewrite where fn is identity outside the
      region`, at two cell sizes, over four boxes, comparing `serialize()`.
- [x] 3.2 The boxes are deliberately OFF the brick lattice. An aligned box
      would never put a written sample and an unwritten one on the same brick
      face, which is the case the whole correctness argument is about.
- [x] 3.3 The test carries its own negative control. The first attempt at one
      passed a region less than half the width of the acting set and still got
      a byte-identical result — the outward rounding absorbed it — so the
      control breaks the precondition outright instead. A parity test that
      cannot fail is not a test.
- [x] 3.4 `a region-limited relax touches nothing outside its taper`: three
      passes, every sample beyond the tuned taper equal to the input's, and a
      count of what changed inside so the first half cannot pass by relax
      having become a no-op.
- [x] 3.5 Full unit suite: 1413 cases, no failures.

## 4. Measured

- [x] 4.1 cell 0.01, `radius_cells` 1, one pass: 16.714 -> 2.234 ms at brush
      radius 0.05 (7.5x), 18.613 -> 5.881 at 0.20, 23.498 -> 13.680 at 0.40.
- [x] 4.2 Four passes at radius 0.20: 72.453 -> 21.137 ms, 3.4x.
- [x] 4.3 Whole-field relax unchanged (71.543 -> 73.329), which is the control:
      with no region there is nothing to bound and nothing should move.

## 5. Not in this change

- [ ] 5.1 `shrink_band`'s global far-bounds chamfer (0.571 ms) and the per-pass
      volume copy (0.162 ms) — together 33% of a small dab at cell 0.01 now
      that the traversal is local. The local far-bounds form is sound but NOT
      byte-identical to the global rebuild, which is an argument that deserves
      its own review. #278.
- [ ] 5.2 The kernel-level items from the original plan — stencil caching,
      radius specialization, brick+halo scratchpad, ping-pong buffers,
      `std::function` removal. All were measured against the OLD code, where
      they were noise. They should be re-measured against the residual profile
      rather than carried forward on the old numbers.
