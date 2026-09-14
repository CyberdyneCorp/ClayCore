## Why

**A backend that was never compiled cannot fail the row that claims it.**
v0.113.0 changed kernel math (the circ easings, #543) and was tagged with a
16/16 PASS checklist behind which no CUDA, OpenCL or Vulkan run had happened
(#578). Nothing malfunctioned. `release_check.py`'s `parity` row asserts *every
backend registered in this build matches CPU scalar*, the release build
registered `cpu` and `metal`, and the two absent backends passed by being
absent.

The substance was fixed in #580 — CUDA and OpenCL both refused a gradients-only
`eval_points`, a real defect the gate found the moment it was run on an RTX
5060. What is left is the process that let a release say more than it knew.

The row's detail made it unreadable in the other direction too. It printed a
**test-case count**, and `-tc=*parity*,*registry*` reports the same case count
with two GPUs attached and with none:

```
main  : [PASS] parity: test cases:      46 |      46 passed
branch: [PASS] parity: compared cpu | 46 cases | 1411932 assertions | NOT BUILT, so this row says nothing about them: cuda, opencl, vulkan
```

## What Changes

- **The parity row names the backends it compared**, from the
  `PARITY_BACKENDS_CHECKED:` line the suite already prints, and names the
  hardware backends missing from the build. A run that reports no such line
  **fails** the row: silence about what was compared is not a result.
- **The four manual hardware gates get four rows.** CUDA device parity, the
  nvcc build with its architecture auto-detection, OpenCL on a real device, and
  Vulkan on real silicon were named in `docs/RELEASE.md` prose only. They are now
  answered from `tests/hardware/manual-gates.json` — each **run**, with
  evidence, or **waived**, with a reason, against a commit — and a gate whose
  commit predates a change to the kernels, or (for the three parity gates) to
  the parity corpus, is stale and fails.
- **A waiver passes.** The row exists to make a release state which of the four
  it ran and which it is shipping without, not to conjure hardware; a gate that
  cannot be satisfied is one people learn to ignore.
- The release order of operations carries the four gates in
  `docs/RELEASE.md` and in `.claude/skills/claycore-release/SKILL.md`, beside the
  device gate, with the numbers to read: **543** assertions cpu-only, **1082**
  with CUDA, **1621** with CUDA and OpenCL, measured on an RTX 5060 at
  `e8ca6d59`. The GPUs contribute **539 each**, not 543 — four assertions are
  CPU-only tolerance branches — so there is no clean multiplier, and a releaser
  who is told to expect one cannot tell a grown corpus from a wrong number.

## What this costs, stated rather than discovered

**Three of the four rows are RED on landing**, and that is the finding rather
than a defect. The recorded RTX 5060 run is at `e8ca6d59`; #582 then added the
circ easings to the list `tests/unit/test_parity.cpp` actually reads — so the
recorded run compared a corpus that did not contain the family #578 was filed
about. The next release must re-run those three on hardware or record a waiver
saying it is shipping without them. `nvcc-build` stays green: a test file cannot
change whether nvcc compiles the backend or which architecture it selects, and a
gate that fails for a reason it cannot be about is one people re-stamp without
reading.

## Not in scope

Making CI run CUDA, OpenCL or Vulkan. The hardware is not there, which is the
premise of the whole change: what a release can do is say so.
