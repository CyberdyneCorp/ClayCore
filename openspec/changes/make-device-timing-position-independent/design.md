# Design

## What the canary is

A fixed workload with three properties, in order of importance:

1. **Independent of everything under test.** If it shared a path with a
   budgeted case, an engine change would move both and the canary would report
   drift that is really a code change. A dense fixed-size buffer walk with
   arithmetic — no document, no grid, no tape — satisfies this.
2. **Cheap.** It runs at least three times per run and buys no coverage. Tens
   of milliseconds, not hundreds.
3. **Stable enough to resolve the effect.** The effect being detected is 2.7x.
   A canary whose own run-to-run spread is 10% resolves that comfortably; one
   that swings 2x resolves nothing.

Deliberately NOT reusing an existing case as the canary. `voxel_add_level` is
tempting — it read 0.53 ms against 0.53 ms across two runs — but it is
budgeted, so an engine change to the level stack would move the canary and the
gate would call it drift.

## Why a spread and not a model

The honest claim available from three samples is "the machine was not the same
at the end as at the beginning, by this much". Turning that into a correction
factor — normalising every case by its offset — is tempting and wrong at this
stage: it would bake an assumed shape for the drift curve into every number,
and nothing measured so far says the curve is linear, monotonic, or the same
across cases. Report the spread; correct nothing.

## Why per-case thermal as well as the canary

They answer different questions and neither replaces the other. The canary says
the machine moved; the per-case thermal state says whether the OS noticed.
Their disagreement is the finding this change exists to make visible — both
ordering runs read `nominal` at every point while a case moved 2.7x.

## The threshold

Recorded rather than derived, and deliberately loose to begin with. The purpose
of the first release of this is to COLLECT the number: three samples per run
across a handful of runs will say what a steady device looks like, and the
threshold can be set from that rather than from a guess. Setting it tight now
would produce a gate that cries drift on every run, which is how a check gets
ignored.

## Where it lives

`Shared/`, because all three bundles need it: the canary has to sample the run,
and after the split the run spans three processes. Each bundle contributes its
own samples with offsets measured from a start the collector already stamps,
and `collect_device_bench.py` merges them the way it already merges one
attachment per test class.

## Rejected

- **Normalising budgets by position.** Above: assumes a drift shape nothing has
  measured.
- **Refusing a drifting run outright.** The existing thermal refusal already
  does this for `serious`, and it is right there. Doing it for canary drift
  before knowing what a steady run looks like would refuse most runs and make
  the gate unusable. Report first; refuse when the number is known.
- **Reading a temperature sensor.** iOS exposes no such API to a test bundle.
  `thermalState` is the only OS signal available, and its coarseness is the
  problem being worked around.
