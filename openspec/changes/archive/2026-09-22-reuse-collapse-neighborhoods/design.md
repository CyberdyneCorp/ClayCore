## Context

The remesher processes edges in stable slot order. Collapse decisions must retain
their current refusal precedence and floating-point calculations. The new recorded
stroke API from PR #617 is part of this change's performance baseline.

## Candidate design

Gather each endpoint fan once into CollapsePlan, derive its existing ordered
neighbor list and incident face list, then reuse those lists during validation.
No topology changes during planning, so the snapshot remains valid. Reuse it in
the pre-write bookkeeping only while connectivity is still unchanged. Invalidate
it after rewiring: post-edit normals and outgoing handles use current topology.

Compare the planning profile with other remaining costs before retaining the
candidate. A result must reduce actual recorded-stroke cost, preserve exact
surface output and operation/refusal counts, and pass undo/redo and constraints.

## Validation

Allocation regression measured on main before the optimization; topology fuzz,
constraints, remesher, dynamic replay and C ABI replay tests; ASan/UBSan; paired
release measurements with recorded gestures at 1k and 10k footprints. Check cold
and evolving states separately where the probe permits, and do not claim a
universal 16 ms bound from this fixture. Measure new/modified helper complexity.
