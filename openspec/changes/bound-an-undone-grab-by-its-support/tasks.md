## 1. Reproduce on this engine, before building

- [x] 1.1 Probe `benchmarks/undo_grab_bound_probe.cpp` (C ABI only, so one
      source builds against main and against this branch): a sphere node of
      radius 1.5, N grabs of radius 0.2 around its equator, one grab of radius
      0.15 on its pole; undo the pole grab, mark the reported bound, refill it.
      On origin/main the bound IS the node's: 1,000 / 1,440 / 4,000 bricks at N = 1 / 10
      / 40 -- growing with the chain, because every grab dilates the node's box
      by its pull.
- [x] 1.2 The acceptance tests written first and run against main: the box and
      count tests FAIL there (5 of 14 cases, 10 assertions: 1,000 / 32,768 / 61,952 bricks
      for one 12-brick grab on a radius-1.5 node, a radius-6 node and a radius-6
      node with 40 grabs; magnify 384 = node, blob 216 = node, alpha 1,000 =
      node, the host's own Move 1,440 = node); the brick oracles PASS there,
      as they must -- the node's bound is loose, not tight, and the oracles are
      the never-tighter guard this change has to keep passing.

## 2. The rule, in the scene layer

- [x] 2.1 `placed_local_bound`: `geometry_bound`'s body with the local box as a
      parameter; `geometry_bound` calls it with `item_local_bounds`
- [x] 2.2 `deformer_head_reach_in_document` (bounds.h/.cpp): tail stripping,
      the qualifying kinds, the easing rim, repetition, placement, groups,
      folds, sharers
- [x] 2.3 `command_head_delta_bound` (commands.h/.cpp), and `UndoStack::replay`
      clamping it into each command's before/after union
- [x] 2.4 Radial pose REMOVED from the qualifying list after the raw check
      found it moves outside its ball (design.md D2)

## 3. Tests

- [x] 3.1 `tests/unit/test_c_undo_bound_grab_support.cpp` -- through the C ABI:
      the box (both ends, fold support, inside the node), redo symmetry, the
      brick COUNT independent of node size and chain length, the brick oracle
      (seam, mirror, group, fold, intersect, ball crossing the node's box,
      magnify, blob, alpha, a mixed step, the host's own Move), and the
      refusals marking exactly the node's bricks
- [x] 3.2 `tests/unit/test_deformer_head_reach.cpp` -- the raw field is
      bit-identical outside the box on the reference evaluator, and every
      refusal
- [x] 3.3 The oracle rebuilds on a COPY of the document (design.md D8)

## 4. Mutation checks

- [x] 4.1 Drop the fold dilation (`layer_reach_in_document` skipped) -> fails
      the C box test and the C++ group-and-fold test (2 cases)
- [x] 4.2 Drop the mirror copies (`with_copies=false`) -> fails the C mirror
      oracle (stale bricks on the reflected side) and the C++ raw check (2 cases)
- [x] 4.3 Report only the centre ball -> fails the C box test, the C++
      displaced-end test and the group-and-fold extent (3 cases). No brick
      oracle can catch this one, by design: the far end is margin (design.md D4)

## 5. Documentation

- [x] 5.1 `bindings/c/clay.h` above `clay_document_undo_bound`: what a deformer
      step reports, and what it does not promise
- [x] 5.2 `docs/05-claycore-library.md`, `docs/06-host-gpu-previews.md`
- [x] 5.3 No pyclay docstring states the rule (checked); no new symbol, so no
      version bump -- ABI stays 0.120.0

## 6. Gates

- [x] 6.1 ctest, fresh `cpu-only` with tests and pyclay BUILT: 11/11 (the four
      unit shards, pyclay_pytest, the rest), before and after merging #637; the
      new files add 29 cases
- [x] 6.2 `check_c_abi.py build/cpu-only/libclay_shared.dylib`: OK (hygiene +
      ctypes FFI); `check_binding_parity.py --require-import`: OK, `imported
      .../pyclay.cpython-311-darwin.so`; `check_task_symbols.py`: OK;
      `check_test_shards.py`: 2,885 cases over 4 shards
- [x] 6.3 `openspec@1.12.0 validate --all --strict`: 65 passed after the
      merge (66 before; #637 archived its change)
- [x] 6.4 `release_check.py --skip-slow`: every row PASS except `device`, which
      is stale by construction for a `src/` change (and was already stale on
      origin/main: `clay.h` and `pyclay_module.cpp` changed since the gate ran
      at `704f2d4af`). Mac numbers are not device numbers; nothing here claims
      one
- [x] 6.5 Cognitive complexity, clang-tidy against this tree's
      compile_commands.json: every new or changed function <= 15 --
      `deformer_head_reach_in_document` 14, `same_link` 10, `chain_head_change`
      8, `replay` 6; `placed_local_bound` 5 after the symmetry copies moved into
      two helpers (the same body, measured before that split, scored 16);
      test helpers and doctest bodies <= 15

## What building it found

- **Radial pose does not qualify, although `link_support` lists it as finite.**
  The first design had it. The raw bit-for-bit check found 1,917 of 68,796
  lattice points outside its ball moving: `cpose_point` has no zero-weight
  early-out, so past the ball it returns `centre + (p - centre)` rotated by
  zero, which is not `p` in float. Grab and magnify early-out to `p`; blob and
  alpha add exactly 0. Finite WEIGHT is not the property; an untouched RETURN
  is.
- **The easing is half the condition.** `ease_out_sine` is not zero at the rim
  on the host, and a transcendental rim is not guaranteed zero on a GPU even
  where it is on the host. The sines, `out_expo`, the circs, `in_bounce` and
  `in_out_bounce` keep the node's bound.
- **A same-document brick oracle cannot see this failure.** The first oracle
  rebuilt the "fresh" cache on the undone document itself. With the mirror
  image dropped from the bound it reported ZERO stale bricks: the seed store is
  the document's, and the rebuild resumed from the very seeds the narrow bound
  had failed to drop. Rebuilding on a `clay_document_save_memory` / `clay_document_load_memory` copy, the same
  mutation reads 8 stale bricks. `test_intersect_delta_oracle.cpp` (#471)
  rebuilds on the same document and so has the same blind spot; it is noted,
  not changed, here.
- **The acceptance's two box clauses conflict for a ball that pokes out of the
  node.** "Covers the support at both ends, dilated by the folds" and "no
  larger than the node's bound" cannot both hold when the grab's ball reaches
  past the node's influence bound. The bound is clamped into the node's (see
  "What review found" for why clamped and not intersected), and the test
  asserts coverage of the support INTERSECTED with the node's bound.
- **The displaced end and the dilations are margin, not soundness.** The raw
  identity argument needs only the centre ball; they are kept so an undo never
  reports less than the live Move reported for the same segment, and so the
  answer stays inside the band-clamped framework the rest of bounds.cpp uses.
- **The chain-length factor stays, measured:** 7.4 -> 28.1 us per refilled
  brick from 1 to 160 grabs on the probe's fixture. Not attempted here.
- **Merge chain:** the 5-hour wait for #635, #636, #638 and #637 hit its cap
  with #637 (per-operator bound narrowing) still open, so this branch started
  from origin/main at `49f4418a` (#635, #636, #638 merged). #637 merged minutes
  later and was merged in (`git merge origin/main`, no conflicts): it narrows
  `tape.bounds` (`combine_extent`), not the reach functions this change uses,
  so the rules stay one rule. Every gate below was re-run after that merge.
- **The task-symbol gate caught this file.** Writing the seed-store finding as
  the short names save_memory / load_memory failed `check_task_symbols.py` (2 unresolved);
  the full names resolve.
