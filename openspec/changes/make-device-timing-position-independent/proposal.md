# Make a device case mean the same thing wherever it runs

## Why

A case's number depends on its position in the suite, by more than the gate's
own tolerance, and nothing in the record says so.

Moving one test bundle from third to first, with no other change:

| case | ran 3rd | ran 1st | before the suite was split |
|---|---|---|---|
| `sdf_stamp_cpu` | 14.988 ms | 5.618 ms | 5.829 ms |
| `sdf_stamp_bricks` | 6.506 ms | 3.065 ms | 3.099 ms |

Same commit, same fixtures, same device. **2.7x**, against a REGRESSION
tolerance of **1.4x** — so ordering alone can report a regression twice the size
of the one the gate exists to catch, or hide a real one of the same size.

And the harness cannot see it. Both runs recorded `thermalStateStart` and
`thermalStateEnd` as `nominal`. `ProcessInfo.thermalState` has four coarse
levels and an M3 iPad throttles measurably inside `nominal`, so the guard that
exists for exactly this — *"a throttled run is a different experiment, not a
slower result"* — passed both.

This is not hypothetical damage. It has already cost:

- A day spent bisecting a `sdf_consolidate` regression that did not exist —
  423 ms one morning, 621 ms the same afternoon on identical code, on the Mac
  and on the device, before the statistic was suspected.
- A budget derivation that had to be thrown away, because it captured
  `sdf_stamp_cpu` at 14.988 ms as if that were the engine.

The budgeted cases now run first, which makes the numbers CONSISTENT. It does
not make them position-independent: anything that changes the run's length or
order moves them again, silently, and the next person to derive budgets will
capture whatever the ordering happened to be that day.

## What changes

The run record gains the context a reader needs to tell a slow engine from a
slow moment, and the gate gains a way to detect drift that `thermalState`
cannot express:

- **Per-case context.** Each case records when it ran — offset from the start
  of the run — and the thermal state at its own start and end, rather than only
  the run's.
- **A canary.** One fixed, cheap workload measured at intervals through the
  run. It exercises no verb under test and has no budget; its only job is to
  say whether the machine underneath the suite changed while the suite ran.
- **A drift verdict.** `check_device_bench.py` compares the canary's samples
  and reports the spread. A run whose canary drifts past a threshold is called
  what it is — a run where position mattered — instead of being silently
  compared against budgets taken under different conditions.

## Non-goals

- **No budget changes.** This change does not re-derive, raise or lower a
  single budget. It makes the conditions visible; deciding what to do about a
  drifting run is the next change, and needs this one's data first.
- **No engine work.** Nothing here makes the device faster or cooler.
- **No new bundle split or reordering.** The three-bundle layout and the
  budgeted-cases-first ordering stay exactly as they are.
- **Not a thermal model.** The canary reports that the machine moved, not why.
  Attributing it to temperature, DVFS, contention or anything else is out of
  scope and would be a guess.
- **No change to what any case measures.** The verbs, fixtures and axes are
  untouched.
