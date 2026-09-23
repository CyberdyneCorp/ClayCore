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

- [ ] 2.1 `placed_local_bound`: `geometry_bound`'s body with the local box as a
      parameter; `geometry_bound` calls it with `item_local_bounds`
- [ ] 2.2 `deformer_head_reach_in_document` (bounds.h/.cpp): tail stripping,
      the qualifying kinds, the easing rim, repetition, placement, groups,
      folds, sharers
- [ ] 2.3 `command_head_delta_bound` (commands.h/.cpp), and `UndoStack::replay`
      clipping each command's before/after union to it
- [ ] 2.4 Radial pose REMOVED from the qualifying list after the raw check
      found it moves outside its ball (design.md D2)

## 3. Tests

- [ ] 3.1 `tests/unit/test_c_undo_bound_grab_support.cpp` -- through the C ABI:
      the box (both ends, fold support, inside the node), redo symmetry, the
      brick COUNT independent of node size and chain length, the brick oracle
      (seam, mirror, group, fold, intersect, ball crossing the node's box,
      magnify, blob, alpha, a mixed step, the host's own Move), and the
      refusals marking exactly the node's bricks
- [ ] 3.2 `tests/unit/test_deformer_head_reach.cpp` -- the raw field is
      bit-identical outside the box on the reference evaluator, and every
      refusal
- [ ] 3.3 The oracle rebuilds on a COPY of the document (design.md D8)

## 4. Mutation checks

- [ ] 4.1 Drop the fold dilation -> MUT_FOLD
- [ ] 4.2 Drop the mirror copies -> MUT_MIRROR
- [ ] 4.3 Report only the centre ball -> MUT_END

## 5. Documentation

- [ ] 5.1 `bindings/c/clay.h` above `clay_document_undo_bound`: what a deformer
      step reports, and what it does not promise
- [ ] 5.2 `docs/05-claycore-library.md`, `docs/06-host-gpu-previews.md`
- [ ] 5.3 No pyclay docstring states the rule (checked); no new symbol, so no
      version bump -- ABI stays 0.120.0

## 6. Gates

GATES_PLACEHOLDER

## What building it found

FOUND_PLACEHOLDER
