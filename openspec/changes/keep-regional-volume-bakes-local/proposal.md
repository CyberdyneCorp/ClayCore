## Why

Issue #595 shows a stationary patch's maintenance growing from 80 ms to 4.1 s
across twelve bakes. The influence closure absorbs an earlier volume's whole
extent, including conservative deformer padding, so maintenance expands its
own future workload and eventually consumes unrelated parametric roots.

## What Changes

- Retain the unaffected samples of a compatible volume and bake only the
  requested region plus the support of the local deformers being removed.
- Stitch the redistanced patch into the retained lattice with a transition
  outside the edited support, preserving colour and avoiding new scene roots.
- Keep conservative whole-root consolidation for combinations for which partial
  replacement cannot preserve the field. Report the plan actually executed.
- Add repeated-maintenance, surface/normal, colour, persistence, undo, and
  cancellation regressions, plus an executable maintenance-only probe.

## Impact

Scene-model regional consolidation and sampled-volume assembly. No new kernel
opcode or document payload format: the result is still an ordinary volume.
Retaining samples may copy existing storage; it must avoid resampling and
redistancing the retained extent, and its cost will be measured separately.
