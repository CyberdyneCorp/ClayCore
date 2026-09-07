## 1. Reproduce before designing

- [x] 1.1 Independent repro against a dense hierarchy as oracle: positions
      identical at 0.000000000, normals wrong by 0.406 / 0.209 / 0.103 at levels
      1 / 2 / 3
- [x] 1.2 The cause, not the symptom: `level_normals` sums `conn.faces_of(v)`,
      and a regional boundary vertex has an incomplete ring at that level
- [x] 1.3 The coefficient gate: `(0.013, -0.021, 0.034)` authored at every
      corner reconstructs up to 0.0072 away from the dense hierarchy, 17% of its
      own magnitude

## 2. One neighbourhood, not two

- [x] 2.1 A parallel level-halo struct was built here independently and is
      DISCARDED -- deleted outright, which is why it is described rather than
      cited. `CrossLevelNeighborhood` already derives the same faces over the
      same joined numbering, and two derivations of one concept is what this
      milestone names in its first sentence
- [x] 2.2 `cross_newell_sum` — raw Newell over `face_corners()` and
      `position()`, added before normalizing
- [x] 2.3 `cross_level_of` — an internal accessor, because `cross_level_at`
      begins with `evaluate_up_to` and would recurse into the running evaluation
- [x] 2.4 Threaded through the six sites that produce a level's normals; the two
      in `refresh_base_frames` stay null because level 0 is never regional
- [x] 2.5 All four callers NAMED in the comment, with `evaluate_up_to`'s loop
      order as what holds the precondition and the one call outside the loop
      that holds it for the other reason

## 3. The weighting

- [x] 3.1 NOT `normal_contribution`: it normalizes and angle-weights, which is
      what the brush's `class_normal` wants and is a different quantity
- [x] 3.2 MEASURED that the choice is observable: 18.27° / 9.48° / 4.47° on a
      curved cage, and exactly 0.000000 on a planar one however graded
- [x] 3.3 Curvature is the discriminating property, not unequal areas

## 4. Release

- [x] 4.1 `release_cross_levels()` — the neighbourhoods and nothing else
- [x] 4.2 No `cache_generation` bump: nothing a bound sculptor references moves
- [x] 4.3 The rebuild cost stated at the declaration rather than discovered
- [x] 4.4 A `cross_released` mark was added and REMOVED: the release nulls the
      pointer, so "released" and "never built" are one state, and
      `level_is_self_contained` already decides "never had one" from the
      topology. A mechanism whose removal changes no observable behaviour is not
      a safeguard

## 5. Gates, and what it took to make them able to fail

- [x] 5.1 Boundary normals against the dense oracle at levels 1–3
- [x] 5.2 The coefficient-reconstruction gate — the one that says it reached
      storage rather than shading
- [x] 5.3 A uniform hierarchy unchanged
- [x] 5.4 The memory test measures the FALL across the release, `floor_bytes`
      from the CSR sizes rather than from `bytes()`, plus positions AND normals
      bit-identical across the trim
- [x] 5.5 THREE GATES PASSED WITH THE FIX DELETED before one worked: one broke a
      path the test did not take; one compared POSITIONS, which cannot detect a
      wrong frame because the frame that writes a coefficient and the frame that
      reads it back are the same frame; one asserted a flag that could not fail
- [x] 5.6 The bound sits between the measured noise floor (4.5e-07) and the
      measured wrong answer (7.1e-02), and both are stated. Exact equality is
      NOT required and would fail a correct implementation: the two hierarchies
      sum the same faces in different orders

## 6. Verification

- [x] 6.1 Full suite on the parent branch with the port applied: 9/9, zero
      build errors, 33 of 33 regional cases
- [ ] 6.2 Rebased onto `finish-regional-multires` once it is on main
- [ ] 6.3 `python3 tools/release_check.py --skip-slow`
- [ ] 6.4 CI green
