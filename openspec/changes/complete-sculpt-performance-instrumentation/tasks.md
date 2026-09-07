## 1. Inventory what is actually there

- [x] 1.1 `SculptStage` names thirteen stages and `StageTelemetry` collects
      nanoseconds and calls per stage, with no clock read when null. That half
      is done and is not rebuilt
- [x] 1.2 AUDITED rather than assumed: exactly ONE consumer in the whole tree
      (`benchmarks/bench_extreme_poly.cpp`), no C entry point, no pyclay, no
      test, and `DynamicSculptor` carrying no telemetry pointer at all
- [x] 1.3 The counts named by the milestone exist scattered across six
      accessors with six shapes; they are collected rather than invented

## 2. Counts beside the clocks

- [x] 2.1 `mesh::SculptCounters`, borrowed and null by default
- [x] 2.2 Considered vs affected kept APART: the gap is the falloff's rim plus
      whatever a mask held still, and it is the first thing to look at
- [x] 2.3 `faces_touched` walked from the write region, NOT taken from
      `refit_tris_` — that list is filled only when a ray tree exists, and
      `surface_index` deliberately never builds one, so the obvious source
      reads zero for a stamp that moved hundreds
- [x] 2.4 The walk happens only when something is counting

## 3. The third representation

- [x] 3.1 `DynamicSculptor` gets stage timings: topology, spatial query,
      neighbour build, snapshot, kernel, writeback, normal refresh
- [x] 3.2 ...and the counts, with splits/collapses/flips through one
      `count_remesh` because `stamp_impl` has two exits and a remesh can run at
      either
- [x] 3.3 The same vocabulary on all three, so a row from each compares. A
      stage a representation does not use reports ZERO rather than being
      omitted

## 4. Host reachability

- [x] 4.1 `clay_sculpt_stage_report`, one descriptor carrying times and counts
- [x] 4.2 `clay_mesh_sculptor_set_stage_report_enabled`,
      `clay_mesh_sculptor_stage_report` and
      `clay_mesh_sculptor_reset_stage_report`, with the matching
      `clay_dynamic_sculptor_set_stage_report_enabled`,
      `clay_dynamic_sculptor_stage_report` and
      `clay_dynamic_sculptor_reset_stage_report`
- [x] 4.3 `stage_count` is what the ENGINE filled, so a caller compiled against
      a newer header reads a smaller number rather than trailing zeroes it
      cannot tell from measured ones
- [x] 4.4 One filler for both representations, so they cannot drift
- [x] 4.5 pyclay: `set_stage_report_enabled`, `reset_stage_report`,
      `stage_report`
- [x] 4.6 ABI 0.91.0 -> 0.92.0 across the three version lines

## 5. Gates

- [x] 5.1 THE COUNTS, not the clocks: a count of splits is the same integer on
      every machine, a duration is a claim about the machine that ran it
- [x] 5.2 One time assertion — a stage that ran took some — which is what
      catches a `StageTimer` deleted in a refactor
- [x] 5.3 Off by default is asserted, not assumed
- [x] 5.4 A reset measures the next dab on its own
- [x] 5.5 PROVEN TO CATCH ITS REGRESSION: with the mesh Kernel timer and the
      adaptive Topology timer detached, two cases fail on three assertions
- [x] 5.6 pyclay covers the same

## 6. Verification

- [ ] 6.1 Full unit suite green
- [ ] 6.2 `python3 tools/release_check.py --skip-slow`
- [ ] 6.3 CI green
