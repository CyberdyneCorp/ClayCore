# Tasks: report-voxel-edit-effect

## 1. The counter

- [x] 1.1 `VoxelGrid::change_count()` plus a `std::uint64_t` member, documented
      as monotone and meaningful only as a difference
- [x] 1.2 One compare in `VoxelGrid::set`, the sole mutation funnel for the
      class — so every verb is instrumented at once rather than eleven verbs
      growing eleven counters. The existing early return (missing chunk,
      index 0) already means "no change" and stays as it is.
- [x] 1.3 Rewriting a cell with the index it already holds is NOT a change, so
      a palette recolour, which touches no voxel data, does not move it either

## 2. The boundaries

- [x] 2.1 `clay_voxel_change_count` in the queries block, `uint64_t` rather
      than `size_t` because it is never reset and a 32-bit host would wrap it
      in a long session; NULL out tolerated, as `clay_voxel_occupied_count` and
      `clay_voxel_size` do
- [x] 2.2 `VoxelGrid.change_count` in pyclay, which the parity gate resolves
      through the `clay_voxel_` prefix rule in both directions
- [x] 2.3 `CLAY_ABI_MINOR` 24 -> 25, and 0.25.0 in `CMakeLists.txt` and
      `pyproject.toml` so the release gate's version check agrees

## 3. What the header now says

- [x] 3.1 Grab: the dead zone stated sharply — per-axis rounding, half a cell
      on the largest component as a necessary and not sufficient condition,
      `front_only` doubling it at the centre, and the two things to reach for
      (`clay_voxel_size` to accumulate against, `clay_voxel_change_count` to
      check)
- [x] 3.2 The sculpting-verbs preamble: every verb here can be a valid call
      with no effect, all of them return `CLAY_OK`, and there will be no
      "valid but no effect" result code
- [x] 3.3 Smudge: it shares grab's rounding and so shares its dead zone
- [x] 3.4 The entry point itself: what counts as a change, monotone and never
      reset, the pinch/magnify upper bound, and that it is not `out_applied`'s
      unit

## 4. Regression tests

- [x] 4.1 Core: a 0.42-cell-per-axis grab leaves `serialize()` byte-identical,
      `change_count()` unmoved and `occupied_count()` unchanged; a supra-cell
      one moves both the bytes and the counter. Same pair for `sculpt_smudge`.
- [x] 4.2 Core: a brush far wider than the ball makes grab a near-rigid
      translation, and there `occupied_count()` reads the SAME for the dead
      drag and the live one — the pairing that shows why the pre-existing
      observable could not answer the question
- [x] 4.3 Core: `front_only` widens the dead zone rather than narrowing it — a
      displacement that moves material ungated moves none with the gate on
- [x] 4.4 Core: rewriting a cell with the index it already holds, and erasing a
      cell in a chunk that does not exist, do not move the counter
- [x] 4.5 ABI: a sub-cell grab returns `CLAY_OK` with a zero delta and a
      supra-cell one with a non-zero delta, the counter agrees with the
      reference `VoxelGrid`, a NULL grid is refused and a NULL out is tolerated

## 5. Documentation

- [x] 5.1 `docs/07-brushes-and-features.md`: the grab row's sub-cell caveat,
      and a note under the verb table that any verb can be a valid no-op with
      how to tell
- [x] 5.2 `docs/05-claycore-library.md`: the `sculpt_grab` example accumulates
      to half a cell
- [x] 5.3 `docs/RELEASE.md`: a 0.25.0 paragraph — additive, one symbol, no
      struct changed size
