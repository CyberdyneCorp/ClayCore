# Tasks

## 1. The session

- [x] 1.1 `reference/host_loop.py` with the six phases, each asserting.
- [x] 1.2 Every one of the 24 sequencing entry points exercised, and the
      in-file coverage check that fails when one is dropped.
- [x] 1.3 Verify the check bites: remove a call, confirm the run fails naming it.

## 2. Wiring

- [ ] 2.1 `CAPABILITY_EXAMPLES` in `examples/run_all.py` gains `reference-host`
      — **at ARCHIVE time, not with this change.** The gate matches the map
      against `openspec/specs/`, and the capability does not exist there until
      the delta is synced, so adding the entry now fails `run_all.py` with
      "names capabilities that no longer exist". Verified by doing it: the
      interlock works in both directions, which is the point of it.
- [x] 2.2 A CI step running the reference host on the bare wheel environment.
- [x] 2.3 `docs/` points at it as the sequencing reference.

## 3. Evidence

- [x] 3.1 Run it; record the phases and the coverage count.
- [x] 3.2 Re-run the gallery and the binding tests, unchanged.

## Evidence

Ten phases, all asserting, 24/24 sequencing entry points covered, ~1 s.
The in-file gate was verified by removing a call and confirming it fails
naming it. 58/58 gallery examples and 386 pyclay tests unchanged.

Three things the session found while being written, all now recorded in it:

- **A ghosted layer refuses `move_layer`.** Protection is checked before the
  operation and a reorder is an operation, so a host must clear protection
  before rearranging the stack.
- **The voxel mirror is about the origin PLANE, not the origin cell.**
  `m.x = -1 - m.x`, so the partner of cell 3 is cell -4. Mirroring to `-x` is
  off by one, symmetrically, and shows up as a one-cell ridge down the middle.
- **`MeshSculptor.refresh`'s docstring is wrong.** It says `raycast` "reports
  the surface as it was when the tree was built"; measured, the hit follows
  the moved surface but through stale BVH bounds, so it drifts OFF the ray —
  6.9e-4 before refresh, 3.1e-9 after. The docstring is not corrected here
  because that is a binding change and this change adds none; it is written
  down so the next person does not trust it either.
