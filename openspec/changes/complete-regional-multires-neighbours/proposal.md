## Why

**`finish-regional-multires` built the cross-level neighbourhood and left the
frame path not reading it**, and said so: sections 1.1–1.5 unticked, with the
cost stated rather than deferred — *124 of 1024 emitted corners carry a
different frame than the dense hierarchy's, worst |dnormal| 0.170116*.

Reproduced independently here, on a 4x4 cage with the four centre patches
refined, compared corner-for-corner against a hierarchy refined everywhere:

| level | corners | worst position error | worst normal error |
|---|---:|---:|---:|
| 1 | 64 | **0.000000000** | **0.406 (23.4°)** |
| 2 | 256 | **0.000000000** | 0.209 (12.0°) |
| 3 | 1024 | **0.000000000** | 0.103 (5.9°) |

The cause is one line: a vertex normal is the sum of `conn.faces_of(v)`, and
beside a refined region half that ring is not stored at this level.

**It reaches storage, not shading.** `P(n) = S(n) + Frame · Detail`, and the
frame is built from the normal. Authoring `(0.013, -0.021, 0.034)` at every
corner of the region in both hierarchies, the regional one reconstructed it up
to **0.0072 away** from where the dense one put it — 17% of the coefficient's
own magnitude. It is reached by SCULPTING near a boundary, not by export.

## What Changes

**The frame path reads the neighbourhood that already exists.** No second
mechanism: `CrossLevelNeighborhood` derives the faces the level does not store,
over a joined numbering, from the same stencils. A `LevelHalo` was independently
built here first and is discarded — two derivations of one concept is what the
milestone this closes names in its first sentence.

- `cross_newell_sum` in `surface_frame.cpp`, threaded through the six evaluation
  sites that produce a level's normals. The two in `refresh_base_frames` stay
  null: level 0 is the cage and is never regional.
- `cross_level_of` — an internal accessor. `cross_level_at` begins with
  `evaluate_up_to`, so calling it from inside the evaluation recurses into the
  evaluation that is running.
- `MultiresSurface::release_cross_levels()` — releases the neighbourhoods and
  nothing else, for the memory test below and for a host under pressure.

## RAW NEWELL, NOT `normal_contribution`, and this is the load-bearing decision

`level_normals` sums `newell(...)` **unnormalized** — its magnitude is twice the
projected area, so the sum is area-weighted by construction.
`CrossLevelNeighborhood::normal_contribution` normalizes each triangle and
weights it by the corner angle, which is what the brush's `class_normal` wants.
It is the function that LOOKS like the answer.

Using it would weight a boundary vertex differently from an interior one.
Measured, both weightings computed over the dense hierarchy's own faces:

| cage | level 1 | level 2 | level 3 |
|---|---:|---:|---:|
| planar, uniform | 0.000000 | 0.000000 | 0.000000 |
| planar, 3.0x graded | 0.000000 | 0.000000 | 0.000000 |
| curved (this fixture) | 0.317 (18.27°) | 0.165 (9.48°) | 0.078 (4.47°) |
| curved, 3.0x graded | 1.173 (71.82°) | 0.787 (46.34°) | 0.198 (11.37°) |

**Curvature is the discriminator, not unequal areas.** A planar cage agrees
exactly however hard it is graded.

## What measuring refuted

Five wrong turns, each a real number correctly taken and reasoned from, and each
wrong in the CHOICE upstream of the measurement.

1. **Incident-area ratio as a proxy** for whether the two weightings can
   disagree. It falls to 1.05x by level 3 on this fixture, which said the
   fixture was blind. It is not: the weightings differ at 1089 of 1089 vertices.
   Area equality is not the property; curvature is.
2. **The wrong vertex set.** 4.47° measured over ALL dense vertices was carried
   to the boundary corners as though it were the same number. At the corners the
   wrong port reads 0.071 (4.05°) at level 3 — SMALLER than the 0.103 defect it
   replaces. A wrong port makes the defect smaller without fixing it.
3. **The wrong precision.** The agreement was reported as exact `0.000000`; that
   was `%.6f`. It is 6.7e-08 to 4.5e-07 — float epsilon from summation ORDER,
   which `cross_level.h` already warns about. Exact equality across two
   summation orders is not achievable and a gate demanding it would fail a
   correct port. The bound sits between the measured noise floor (4.5e-07) and
   the measured wrong answer (7.1e-02), five orders apart.
4. **Three gates that passed with the fix deleted.** One broke the wrong path;
   one compared POSITIONS, which cannot detect a wrong frame because within a
   stamp the frame that writes a coefficient and the frame that reads it back
   are the same frame and the error cancels; one asserted a flag.
5. **A safeguard that could not fail.** A `cross_released` mark was added to
   distinguish "released" from "never had one" — and `release_cross_levels`
   nulls the pointer, so the two are the same state and the mark carries nothing
   observable. `level_is_self_contained` already decides "never had one" from
   the topology. Removed: a mechanism whose removal changes no observable
   behaviour is not a safeguard, and unlike a gate, nobody re-reads a flag.

## Capabilities

### Modified Capabilities
- `mesh-multires`: a regional level's normals and detail frames are the dense
  hierarchy's, and the neighbourhood that makes them so can be released without
  releasing the level.

## Impact

- `include/clay/mesh/surface_frame.h`, `src/mesh/surface_frame.cpp`
- `src/mesh/multires_eval.cpp` — `cross_level_of` and the six sites.
- `include/clay/mesh/multires.h`, `src/mesh/multires.cpp` —
  `release_cross_levels`.
- `tests/unit/test_multires_regional.cpp` — the dense-oracle gates, the
  coefficient-reconstruction gate, and the memory test rewritten to measure the
  fall across the release.
- **Supersedes** `finish-regional-multires`'s spec text describing the frame as
  knowingly wrong. That text was correct when it landed.
- No ABI change, no format change.
