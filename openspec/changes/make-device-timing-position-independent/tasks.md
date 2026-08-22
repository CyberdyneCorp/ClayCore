# Tasks

## 1. The record

- [x] 1.1 `Measurement`/`CaseResult` carry the case's start offset in ms from
      the run's start, and the thermal state at the case's own start and end.
- [x] 1.2 The run-level thermal pair stays. The existing refusal is written
      against it and this change does not touch that behaviour.
- [x] 1.3 `collect_device_bench.py` merges the new fields across the three
      bundles' attachments without losing offsets.

## 2. The canary

- [x] 2.1 A fixed synthetic workload in `Shared/`, touching no verb under test
      and carrying no budget.
- [x] 2.2 Sampled at least three times per run, spanning the first and last
      third, each sample recorded with its offset.
- [ ] 2.3 Verify it is independent: change a verb's cost and confirm the
      canary does not move.

## 3. The verdict

- [x] 3.1 `check_device_bench.py` computes and prints the canary spread on
      every run.
- [x] 3.2 A spread past the threshold is reported as changed conditions, and
      said even when every thermal state read `nominal`.
- [x] 3.3 A budget or regression failure on a drifting run names the drift
      beside it.
- [ ] 3.4 Verify the check bites: run the suite in two orderings and confirm
      the one that moved `sdf_stamp_cpu` 2.7x is reported as drifting.

## 4. The threshold

- [ ] 4.1 Collect canary spreads across several steady runs and record what a
      steady device looks like.
- [ ] 4.2 Set the threshold from that, with the number and its source written
      down rather than chosen.

## 5. Wiring

- [ ] 5.1 `CAPABILITY_EXAMPLES` in `examples/run_all.py` gains `device-gate`
      — at ARCHIVE time, not before. That gate matches the map against
      `openspec/specs/`, where the capability does not exist until the delta is
      synced, so adding it earlier fails `run_all.py`.
- [ ] 5.2 `docs/09-brush-latency-and-coverage.md` records what the canary is
      for, beside the existing note on the 2.7x position effect.

## Evidence

The canary fired on the first run that carried it, against real conditions
rather than an injected fault:

    canary: CONDITIONS CHANGED while this run ran — x1.55 across the run
            (131.62 ms at 1s -> 203.37 ms at 243s, tolerance x1.3)
            ...and thermalState read `nominal` at both

Fifteen samples across three bundles. The measurement bundle's own readings
tell the story on their own — 133.15 ms at its start, 133.32 at 119 s, 135.54
at 171 s, 138.92 at 223 s, then 203.37 at 243 s. The machine was 1.5x slower by
the end of the run than at the beginning, and the OS called it nominal
throughout.

That is the effect the proposal was written from, now measured directly rather
than inferred from two orderings of the suite.

Records without a canary still check: the gate reports that it cannot say
whether position mattered, rather than failing them.

Remaining: 2.3 (prove independence by moving a verb's cost), 3.4 (prove the
check bites on a second ordering), 4.x (collect steady-run spreads and set the
threshold from them), 5.x (wiring at archive time).
