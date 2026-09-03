## 1. The carried anchor (closes add-extreme-poly-runtime 3.1)

- [x] 1.1 SUPERSEDED by the chunk tree — see design.md 2.1/2.2. The carried anchor, retired with the class space, reset
      on a new stroke, range-checked against the radius before use
- [x] 1.2 SUPERSEDED, and measured before it was: 7 of 80 goldens moved. The descent from the carried anchor to the class nearest the
      centre, replacing the scan and NOT the anchor — see design.md §2
- [x] 1.3 Precedence: caller seed, then ray tree, then carried anchor, then scan.
      The two existing fast paths must be bit-identical to today
- [x] 1.4 The same for the other scan sites on the no-index path:
      `nearest_class`'s fallback, the alpha frame's direction fallback, and the
      connectivity automask's anchor
- [x] 1.5 `MultiresSculptor` supplies the level ChunkTable rather than carrying the anchor across the dabs of a stroke and
      drops it on a rebind that retires the class space

## 2. THE GATE the design turns on

- [x] 2.1 `test_mesh_sculpt_parity.cpp` — all 80 cases, hashes AND moved counts,
      UNCHANGED. This is not a regression check, it is the acceptance criterion
      for §2's design: 5 fixtures x 16 verbs x 3 stamps, no ray tree, so every
      case is on the path this change edits
- [x] 2.2 `sheets` DID move under the carried anchor, ship design.md §2.1 instead and record why
- [x] 2.3 A NO-SCAN gate: a stroke of N dabs on a mesh with no ray tree visits a
      class count that follows the FOOTPRINT, not the model. Counted, not timed —
      a timing gate on a shared box proves nothing.
      DONE, `tests/unit/test_chunk_query_locality.cpp`, through
      `MeshSculptor::anchor_measurements()`. 1,960 measurements at 9,409
      vertices and 1,960 at 148,225 — IDENTICAL, not banded, over a 16x model at
      one world footprint
- [x] 2.4 Proven to catch its regression: with the descent disabled the no-scan
      gate must fail, and the revert must COMPILE.
      DONE. With `chunk_index()` forced to null the branch COMPILES (0 errors)
      and the same two rows read 75,272 and 1,185,800 — exactly 9,409 and
      148,225 times the eight dabs, one whole class scan per dab, a ratio of
      15.75 against a model ratio of 16. Four assertions fail across two cases
- [x] 2.5 N/A under the chunk design: a stroke that jumps a full model
      away still moves the vertices a cold stamp would have moved

## 3. Per-stage telemetry (closes 7.2)

- [x] 3.1 One `SculptStage` enum in `sculpt_common.h`, shared by the fixed,
      adaptive and hierarchy paths
- [x] 3.2 The stages inside `MeshSculptor::stamp`: gather, geodesic,
      snapshot, weight, alpha, automask, kernel, normals
- [x] 3.3 Null by default and NO clock read when null
- [x] 3.4 `bench_extreme_poly` prints the eight instead of the `stamp*` bucket.
      DONE, and the first row it produced was already actionable: at 100k
      vertices and a 20k footprint the stamp divides into query 27.0%, weight
      33.5% and normals 33.0%, with kernel at 0.1% — so the verb is not where a
      dab's time goes and three stages that were one bucket are now three

## 4. Chunk marking

- [x] 4.1 `MeshSculptor` borrows a `ChunkTable` and marks geometry over the write
      region, normals when refreshed, attributes when the verb painted
- [x] 4.2 NEVER topology — the fixed-topology contract
- [x] 4.3 The gate: a stamp's dirty stream reconstructs the same surface as
      `to_mesh()`, with the marking done by the sculptor rather than the test
- [x] 4.4 `bench_extreme_poly.cpp` stops marking from outside.
      DONE, and it found the defect: `publish_chunks` required the chunk TREE
      to have been built, which tied the dirty stream to the query path — so a
      caller supplying a table purely for transport, and passing its own
      `seed_class` as a host that picks does, got an empty stream and a silent
      zero-byte upload. The map is what the stream needs, not the tree. Caught
      by the benchmark's upload column reading 0.0 KB

## 5. The benchmark rows (closes 7.1's layered rows)

- [x] 5.1 Run `multires + sculpt layers` at the sizes the matrix names.
      DONE. `bench_extreme_poly` gains `--which=layers`, replacing the printed
      apology that stood where the numbers belonged. The locality property
      holds on the fourth path: 10x the model at one footprint is 1.08x, 1.12x
      and 1.15x, with dirty chunks and upload IDENTICAL at both sizes.
      Adding the row also found a defect in the WRAPPER: its `REPRESENTATION`
      regex matched `hierarchy` followed by a colon, so `hierarchy + layers:`
      fell through and the layered rows were attributed to the previous
      representation, overwriting its entries under the same key -- silently,
      because every field still parsed. Longest alternative first. Through the
      fixed wrapper the layered stamp reads 1.14x p50 for 10x the model at load
      3.36 -> 3.25
- [x] 5.2 Record in `docs/09-brush-latency-and-coverage.md` with the load the box
      carried, per shared-box practice — ratios, not absolutes.
      DONE, load 6.81 before the pair and 6.42 after, the two halves run back to
      back so the ratio between them is not one across two sessions. The finding
      worth carrying: a layered gesture holds the composition, so the per-dab
      detail write goes from 2,494 us to 0.14 us and a non-destructive pass is
      CHEAPER per dab at a 20k footprint (0.92x) and dearer at 1k (1.23x). The
      commit is deliberately not in those numbers and the doc says so

## 6. Gates

- [x] 6.1 `cpu-only` green; `check_layering.py`, `check_binding_parity.py`,
      `check_c_abi.py` green.
      DONE. 2,313 cases / 16,367,763 assertions, 0 failed (2,310 on main plus
      this change's 3). layering OK, parity OK (716 pyclay capabilities),
      c-abi OK. pyclay: 681 passed, 1 skipped, under
      `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6` — the anaconda
      GLIBCXX_3.4.31 quirk this box has, not a result of this change
- [x] 6.2 No ABI change, so no version bump.
      `release_check.py` fails four rows and NONE of them are this change,
      verified rather than asserted: `bindings`, `abi` and `wheel` are all the
      same anaconda GLIBCXX ImportError, and the 681 pyclay tests pass under the
      preload; `device` says "engine changed since the gate ran at 9ed0a49a9",
      and main itself already differs from that commit by 843 lines in
      clay.h/clay_c.cpp, so it fails on main too. `version`, `configure`,
      `build`, `parity`, `layering`, `dialect`, `licenses`, `kernels`,
      `openspec` and `benchmarks` all pass
- [x] 6.3 7.8 (reference iPad) STAYS OPEN — no device on this box, and nothing
      here claims a device number
