## 1. The placement

- [x] 1.1 `mesh_mask_gate` places a vertex with `mesh_to_world`;
      `mesh_automask_inputs`, three lines below it, did not
- [x] 1.2 CONFIRMED ON `main` before calling it pre-existing
- [x] 1.3 Reachable from pyclay today, which is why this is not waiting behind
      the C ABI change that found it — `MeshStrokeOptions` has carried
      `mesh_to_world` since it shipped
- [x] 1.4 Both lambdas place the point; no new field, no new plumbing, the same
      shape `mesh_mask_gate` already had

## 2. The strength

- [x] 2.1 `read_mesh_brush` read zero as "unset, take the engine default", which
      is right for the two appended scalars beside it and wrong for a slider
- [x] 2.2 The observable path is `read_brush_preset`, which comes through here:
      a preset saved with the cavity slider at zero deserialized at FULL
- [x] 2.3 The stamp could NOT see it — the factor is inert from C — so a test
      that stamped would have passed with the bug in place
- [x] 2.4 Documented default becomes zero; `clay_mesh_brush_defaults` supplies
      the engine's 1.0

## 3. Tests

- [x] 3.1 `test_mesh_sculpt.cpp`: the cavity field and the group lattice, each
      on a placed layer
- [x] 3.2 `test_c_brush_preset.cpp`: the round trip, at 0, 0.25, 0.5 and 1
- [x] 3.3 Proven by reverting each fix
- [x] 3.4 REFUTED — the placement case on the sphere the existing cavity test
      uses. A sphere is CONVEX, so its cavity is zero placed and unplaced alike
      and the case passed while proving nothing. Caught by its own
      `any_differed` guard; two overlapping spheres now
- [x] 3.5 The strength case asserts the bit crossed too, or a dropped factor
      would zero the strength and read as a pass

## 4. Scope

- [x] 4.1 NOT making CAVITY or SURFACE_GROUP reachable from C: separate change,
      its own entry points, its own version bump
- [x] 4.2 No version bump here — no entry point added, both are fixes restoring
      documented behaviour

## 5. Gates

- [x] 5.1 `cpu-only` suite green
- [x] 5.2 `release_check.py --skip-slow`
