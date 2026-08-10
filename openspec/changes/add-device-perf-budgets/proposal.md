# Proposal: every number we have is from the wrong machine

## Why

The engine's reason for existing is an iPad sculpting app, and the interactive
budget is stated precisely — 4–8 ms per Pencil event at 120–240 Hz, 16.7 ms for
a preview frame at 60 fps. Nothing in the repository measures against it, and
every number that exists was taken somewhere else:

| Recorded | Measured on |
|---|---|
| `raycast_many`, `eval_points`, `BM_EvalPoints` speedups | a desktop |
| Metal vs CPU per brick, the 16³ crossover, the 3.0× fan-out | an M2 Max — 12 cores, active cooling, 34 GB unified |
| A dab's 2.6 → 8.8 ms over document size | a desktop |

`docs/RELEASE.md` is candid about the gap — "What is still untested is a
*tablet*… Re-measure both on the target iPad before wiring up the split" — and
that note has been standing since 0.24.0 while decisions got made on the desktop
numbers anyway. The choice to keep brick fills on the CPU rests on a crossover
measured on a machine with a different GPU, a different dispatch cost and no
thermal ceiling.

Three things follow that are not fixed by measuring once by hand.

**Nothing asserts the budget.** The benchmarks report throughput
(`BM_EvalPoints`, `BM_BrickFill`, `BM_MeshTape`, `BM_SurfaceNets`) with, per the
v1 tasks, "generous floors [that] catch order-of-magnitude regressions". An
order of magnitude is not the resolution this needs: the difference between
6 ms and 9 ms per dab is the difference between shipping and not, and no floor
in the tree would notice it.

**The benchmarks do not measure the thing.** They measure stages. The
interactive path is a sequence — mark dirty, drain requests, evaluate, submit,
and for a preview also mesh or raycast — and its total is what the artist feels.
A stage that got 2× faster while the total got slower is a result this suite
cannot produce.

**Sustained behaviour is unmeasured entirely.** A tablet's interesting number is
not the first dab, it is the dab five minutes in, after the SoC has warmed up
and the OS has started moving work to efficiency cores. Every number here is a
cold-start number.

## What changes

**A harness that measures the interactive path end to end** rather than its
stages: a dab, from gesture to updated bricks, and a preview frame, at stated
document sizes.

**It runs on the target.** arm64, on the device, not in the simulator — the
simulator runs the host's cores and cannot answer a thermal question. The
existing Swift smoke harness already establishes the route onto a real device
through the xcframework.

**A sustained run**, reporting first-dab and steady-state figures separately, so
thermal behaviour is a number rather than an assumption.

**The results live in the repository**, naming the device, the OS and the build,
so "we got slower on iPad" is a diff rather than a memory.

**A stated budget with a stated verdict.** Pass or fail against 4–8 ms and
16.7 ms at a declared document size, not a throughput figure a reader has to
interpret.

## What it is not

**Not a CI gate on device.** There is no iPad in CI and pretending otherwise is
how the CUDA and OpenCL jobs came to test nothing — that lesson is already
recorded in `docs/RELEASE.md`, which now names three checks as manual and
hardware-dependent rather than faking them. This joins that list: run before a
release that touches the interactive path, with its output committed.

**Not a replacement for the microbenchmarks.** They stay. They are how a
regression gets attributed to a stage once the total says something moved.

**Not a promise the budget is met.** It may not be, at some document size. The
point is to know the size, publish it, and let the sculpting-path changes be
judged against it — which is the only way any of the other changes proposed
alongside this one can be said to have worked.

## Why this is its own capability

The budget is a property of the product, not of the build. `build-packaging`
owns the test pyramid and the release checklist and is the right place for *how*
this runs; it is the wrong place to state that a brush dab is expected to
complete in 8 ms on a named device. That is a requirement the engine either
meets or does not, and it deserves to be sayable, testable and citable — which
is what a capability is for.

## Open questions

- **Which device is the reference.** The oldest supported iPad is the honest
  choice and the least convenient one. Whichever it is, it must be named in the
  requirement, because a budget without a device is not a requirement.
- **Which document sizes.** 100 / 2 400 / 10 000 items matches the existing
  measurements and makes the numbers comparable; a real `.clayspace` from actual
  use would be more honest and less reproducible. Possibly both.
- **How the harness gets on the device** — an XCTest performance target, a
  command-line binary run through `devicectl`, or the existing Swift smoke
  program extended. Whichever it is, it must be runnable by one person with one
  command or it will not be run.
- **What a failure means before the budget is achievable.** If the first run
  fails at 10 000 items, the requirement should record the size at which it
  passes today and the size it targets, rather than being weakened until it
  passes.

## Impact

A new `performance-budgets` capability states the budget and what is measured
against it. `build-packaging` gains the release-time obligation. No code
behaviour changes; this change is entirely about knowing whether the others
worked.
