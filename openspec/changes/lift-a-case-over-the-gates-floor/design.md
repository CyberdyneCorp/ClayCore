# Design

## Why batching, and why the record has to say so

A figure of 0.41 ms where the last run said 0.0001 ms is meaningless unless the
record says the timed unit changed. `Measurement.batch` is that statement: how
many applications of the verb ONE timed body performed. It defaults to 1, so
every case that was not touched keeps its meaning and every existing call site
compiles, which is the same additive shape `repeats`, `p95SpreadMs`,
`startedAtMs` and `canaryBeforeMs` already used.

The alternative — grow the per-application workload — is better where a knob
exists, because it keeps "one call, one number". It is why `voxel_smooth_r32`
exists and it is untouched here. It does not generalise: six of the twenty-one
measure cases have no size input that leaves the verb recognisable.

## What batching costs, stated plainly

**A regression confined to one application is diluted.** A verb that got slower
only on its first touch of a brick would be averaged 128:1 and could pass. This
is the mirror of the finding that a device case must exercise the path its verb
names, and it is the strongest argument against batching.

It is survivable for these cases specifically: every one of them is a single
ABI call against a resident fixture, with no first-touch behaviour to hide. It
would NOT be survivable for the brick cases, which is exactly why
`sdf_stamp_bricks` is excluded — see below. If the voxel path ever gains a
cache, this assumption has to be re-checked.

## The two brick cases, which are not alike

`sdf_stamp_bricks` must not be batched. Its reset REMOVES the previous node, so
every timed body starts from a general invalidation and pays a full tape
recompile. K stamps inside one body would let stamps 2..K append onto an
un-invalidated document — the suffix-compile path that `sdf_stroke_bricks`
exists to measure. Batching would make it report a number for a path its verb
does not name. Its axis gains a 10,000-stamp point instead, which is the
instrument its own comments already describe: its cost is (bricks dirtied) x
O(document).

`sdf_stroke_bricks` is the opposite: its timed body has ALWAYS been the whole
24-dab stroke, and it divided by 24 on the way out. That quotient is what put
it under the floor. It was gateable until `perf/device-refill-resume` (#355)
landed and took the 1000-stamp point from 0.164 ms to 0.047 — a 3.5x WIN that
made the case unable to object to a regression. That is issue #337's defect
arriving by the front door, and it is the argument for recording `batch` rather
than a quotient: an optimisation should not be able to switch a gate off.

## Why the drags are the faithful edit, not the cheap one

The gallery's voxel cases are named `session_voxel_*` and their own comments
describe drags. They timed one dab. Timing the whole drag is not a workaround
for the floor — it is what the case was always supposed to measure, and it is
the shape `stroke_build` already had, which is why `stroke_build` measures
0.297 ms and gates while `session_voxel_build` measured 0.0053 ms and could not.

Every drag walks the SAME path deliberately. Two fixtures depend on it:
`fill_cavities`' 24 single-cell pockets lie on the swept curve, and
`mask_freeze`'s mask is painted along one line. Offsetting the drags would take
both cases back to measuring nothing while passing — which two earlier fixtures
in this file already did once.

## move_drags: four sub-steps, and not eight

This is the expensive case in the gallery bundle and the only one whose own
canary bracket reads elevated. At eight stacked warps the layer already reports
a Lipschitz bound of 333 and a safe step scale of 0.003, so the sphere trace
behind its two renders burns its whole iteration budget on every ray, and the
consolidate after them pays the same field. Deepening the chain multiplies all
three.

Four sub-steps clears the floor. Eight would buy a number no more gateable at
several times the wall clock, and would deepen a known pathology rather than
reveal a new one. The total displacement per stroke is unchanged, so the render
shows the same form.

## What was deliberately not changed

The tolerance (1.4x) and the floor (0.05 ms) both stay. They are right, and
buying sensitivity by lowering either would spend the false-failure budget that
#331, #333 and #336 were spent earning: a case that "cannot fail" is a worse
problem when it becomes one that "fails at random".
