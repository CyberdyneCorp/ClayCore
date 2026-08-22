# Tasks

## 1. The record

- [ ] 1.1 `Measurement`/`CaseResult` carry the case's start offset in ms from
      the run's start, and the thermal state at the case's own start and end.
- [ ] 1.2 The run-level thermal pair stays. The existing refusal is written
      against it and this change does not touch that behaviour.
- [ ] 1.3 `collect_device_bench.py` merges the new fields across the three
      bundles' attachments without losing offsets.

## 2. The canary

- [ ] 2.1 A fixed synthetic workload in `Shared/`, touching no verb under test
      and carrying no budget.
- [ ] 2.2 Sampled at least three times per run, spanning the first and last
      third, each sample recorded with its offset.
- [ ] 2.3 Verify it is independent: change a verb's cost and confirm the
      canary does not move.

## 3. The verdict

- [ ] 3.1 `check_device_bench.py` computes and prints the canary spread on
      every run.
- [ ] 3.2 A spread past the threshold is reported as changed conditions, and
      said even when every thermal state read `nominal`.
- [ ] 3.3 A budget or regression failure on a drifting run names the drift
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
