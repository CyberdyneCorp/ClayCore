## Context

`brush::apply_to_mesh` decides what a drag hands the verb in one place —
`mesh_stamp_settings` in `src/brush/stroke.cpp` — and all three mesh consumers
share it, which is the property `stroke-an-adaptive-surface` exists to keep.
Today, for Grab:

```
out.direction = s.position - previous;   // motion since the last stamp
out.center    = first;                   // the stroke's first sample
```

The kernel is one line (`src/mesh/sculpt_kernels.cpp`, `kernel_grab`):

```
out[i] = settings.direction * s.weights[i];
```

and the write-back is `captured_position + displacement` — note that both
`MeshSculptor::write` and `DynamicSculptor::write_positions` write
`region_.positions[i] + displacement_[i]`, the gathered position and not the
current one. That is what makes a captured set expressible at all: with the
region held, `direction = p_k − p_0` places every captured vertex at exactly
`p_gathered + w * total_drag`, with no accumulation error.

So all three candidates are small changes to the same two lines, plus, for B,
one flag saying "do not gather, reuse what you have".

`settings.strength` never reaches `kernel_grab`. Grab's amount is the drag and
the falloff, and nothing else.

## Goals / Non-Goals

**Goals.** One anchoring rule for Grab, applied to `apply_to_mesh`,
`apply_to_multires` and `apply_to_dynamic` together; the two representations
still agreeing to at least the 1e-3 #619 holds; the goldens that move, moving
deliberately.

**Non-Goals.** Snakehook, whose rule was measured in #619 and is right.
The SDF/voxel move brush, whose shortfall is a different mechanism (the region
weight is taken at the sample point rather than its preimage) and is already
documented as deliberate. A latency change on the adaptive path.

## Decisions

### D1. The rule is B: a Grab carries the region it captured

Gather once, at the first stamp. Carry the items, their captured positions and
their weights for the whole gesture. Write `captured + w * (p_k − p_0)`.

Measured, on six fixtures and both paths, against the alternatives
(`proposal.md` §1–§2). The short form:

| | reach, fixed | reach, adaptive | fixed vs adaptive |
|---|---|---|---|
| A, today | 18–41% | 18–42% | 1.0e-3 – 5.4e-3 |
| B, remesher unable to touch the set | **100%** | **100%** | **0.0 – 2.0e-5** |
| B, remesher as today | 100% | 2.8–100% | 0.0 – 3.4e-1 |
| C, following centre | 26–67% | 22–69% | 7.5e-3 – 6.2e-2 |

Reach is a share of the NET DISPLACEMENT `|p_n − p_0|`, which on the curved
fixture is the 0.5402 chord and not the 0.6 arc; see `proposal.md`. A's range
here is for a brush radius of 0.3 — it reads 22.66% at 0.15 and 60.56% at 0.50,
so the constant is the decay with drag length and not the 41%. The B row's
adaptive floor is the whole point of D2 below, and it is a floor measured at
radius 0.15 / detail 4, where the remesher retires the captured set ENTIRELY.

The centre of a captured set has weight 1, so it follows the cursor exactly;
that is what "100%" means and it is what a Move brush is for. It is also 1.85x
cheaper per stroke on the fixed path, because a gesture walks the surface once
instead of once per stamp.

**Why not C.** Two independent refutations, either of which is sufficient. It
disagrees between the representations by 10% of the drag — 40x worse than
today, and #619's whole design rests on that agreement. And on a drag of 1.5 it
applies 11 of 26 stamps and then stops: the centre vertex moves by its falloff
weight rather than the whole delta, so the brush falls behind the cursor and is
soon outside its own radius. `src/brush/stroke.cpp` already documents exactly
this failure for a Snakehook centred on the cursor; it is the same failure.

**Why not A.** It is not a discount, it is a decay: 41% at a 0.6 drag and 18% at
1.5, because each stamp gathers a weaker region than the last. There is no drag
length at which a host can predict what the brush will do.

### D2. On an adaptive surface the captured set is MAINTAINED, not merely held

This is the part the issue does not name and the measurements insist on.

The adaptive surface remeshes after every Grab stamp, and a collapse retires
vertex ids. Counted at the end of an 11-stamp stroke: **7 to 13 of 45 captured
vertices are still live**. Reach then depends on whether the weight-1 centre
happened to survive — 100% on one pole of the sphere, 44% on the other. A rule
with that property is not a rule.

Widened over grid, spacing, radius and detail it is worse than that: at radius
0.15 against detail 4 the remesher retires **all 9** captured entries inside one
stroke and the reach falls to 2.8–7.0%, against today's 9.3–22.3% on the same
fixtures. An unmaintained captured set is not a degraded version of the fix; on
some presets it is a regression.

The control proves the cause is the remesher and nothing else. With
`DynamicTopologySettings::enabled = false`, B is BIT-EXACT between the two
representations (agreement 0.000000, reach 100% on both) — tighter than today's
A, which disagrees by 1.6e-3 to 5.4e-3 because the two gathers differ. With the
remesh held off only inside the gesture (candidate Q), B agrees to 1.0e-6 –
2.0e-5 on all six fixtures and leaves a surface that validates.

So:

1. The remesh publishes its splits and collapses into the workset the gesture is
   carrying. A split inside the captured set inserts the new vertex with the
   midpoint of its parents' CAPTURED positions and the mean of their weights; a
   collapse removes the entry whose vertex it retired. Nothing else changes.
2. The remesh centre follows the stamp rather than staying at the anchor.

(2) is mandatory on its own evidence. Longest edge left on the surface after a
1.5 pull-out: 0.1174 under A, **1.1227 under B with the remesh left at the
anchor** — worse than no remesh at all, because the tip is 1.5 away and the ball
never reaches it — 0.3571 with the centre following. A remesh at the first
stamp's centre is only defensible while the surface barely moves, which is
precisely the behaviour this change removes.

The midpoint-of-captured-positions rule is the only one that composes: a vertex
born mid-gesture is born between two parents that have ALREADY taken part of the
drag, so its captured position must be reconstructed from its parents' captured
positions, not read off the surface. Reading the surface would give it the drag
twice.

Cost: O(splits + collapses) per stamp, which the remesh already walks. Asserted
as a count — captured entries in, entries retired, entries inserted — not as a
duration.

### D3. Rejected: suppress the remesh inside a Grab gesture (candidate Q)

Measured, and it is the cheapest and most accurate thing in the table: reach
100% on both paths on all six fixtures, agreement 1.0e-6 – 2.0e-5, and **2.7 ms
against A's 128 ms on the long pull-out — 47x**, because the remesh is what an
adaptive stroke costs.

Rejected anyway, on one number: the longest edge it leaves after that 1.5 drag
is **0.8176, exactly the fixed mesh's**. A Grab that turns the adaptive surface
into a fixed one for the length of the gesture has given up the thing the
representation exists for, and the single remesh at the tip on the last stamp
refines a ball at the tip and not the stretched corridor behind it.

It is named here rather than dropped because it is the fallback if D2's
maintenance proves larger than it looks, and because it is the second control
that isolates the remesher as B's only problem. If it is ever taken, it must be
taken with that 0.8176 in the release notes.

### D4. Rejected: move the remesh centre and leave the set unmaintained (P)

The cheap half of D2. It recovers the refinement (max edge 0.3571 against
1.1227) and still disagrees between the representations by up to 0.52 on a 1.5
drag, because following the cursor is what retires the captured vertices
fastest — 16 to 19 of 45 survive instead of 5 to 11. Half of D2 is not half the
benefit.

### D5. The multiresolution path follows, and is not separately measured

`apply_to_multires` shares `mesh_stamp_settings` and drives a `MeshSculptor` per
level, so it takes D1 by construction. No test in the tree drives Grab through
it today (`test_multires_sculpt.cpp` uses Draw), which is a gap this change
closes with a case rather than a gap it may ignore.

### D6. No ABI or format movement

No new symbol, no descriptor re-laid out, no scene minor. `CLAY_ABI_*` does not
move. What moves is the documented BEHAVIOUR of
`clay_mesh_sculptor_apply_stroke`, `clay_dynamic_sculptor_apply_stroke` and
their `_preset` and `_recorded` siblings, and the header text beside each of
them, which in this tree is where a host integrator actually reads it.

## Risks / Trade-offs

- **[The captured set stops being the right set on a long drag]** → it is the
  known risk the issue names, and it is real: B drags the same 45 vertices
  however far the stroke goes, so a 1.5 pull is a 45-vertex column rather than a
  swelling neck. Measured as the longest edge (0.8176 on the fixed mesh with no
  remesher, against a starting 0.0917). On the adaptive path D2's maintenance is
  the mitigation — the set grows as the remesher splits inside it. On the FIXED
  path there is no mitigation and there cannot be one: a fixed mesh has no new
  vertices to give. That is what a fixed-topology Grab is, it is what every
  fixed-topology Move brush does, and the change must say so rather than imply
  the fixed path got the same fix.
- **[A Grab now moves the surface 5.6x further on a 1.5 drag]** → every host feel
  tuned against 41% changes. Deliberate, and the reason the issue asks for the
  goldens to be re-derived rather than to drift.
- **[The mesh Grab and the SDF move brush now disagree about reach]** → the spec
  records that the SDF move "moves LESS than the displacement asked for"; after
  this the mesh Grab moves exactly as far as asked. Two brushes an artist thinks
  of as one. Not fixed here — the SDF shortfall is a field-inversion problem, not
  an anchoring one — but stated in `docs/07` beside both.
- **[THE MAINTENANCE IS NOT MEASURED, AND THE INFERENCE TO IT HAS A HOLE]** →
  100% reach and the 2e-5 agreement are Q's and the topology-off control's, with
  the identical deformation rule and a remesher that cannot touch the region.
  They are not the recommendation's, because the recommendation does not exist
  yet. The step from Q to D2 assumes the maintenance keeps a HIGH-WEIGHT entry
  alive, and the rules above do not guarantee one: a split inserts its child with
  the MEAN of its parents' weights, so no maintenance operation can create a
  weight above the surviving maximum, and a collapse of the weight-1 centre is
  irreversible. Where unmaintained B fails, the top surviving weight is 0.090 or
  0.000 — the high-weight core is gone, not only its centre — so the question is
  quantitative: do the splits inside the region repopulate the core faster than
  the collapses retire it? `tasks.md` §2.0 measures that BEFORE the rest of the
  work is built, and names what to do if the answer is no (pin the carried
  region against collapse, or fall back to D3). This is the decision the change
  cannot make on its own evidence.
- **[The remesh maintenance is where the bugs will be]** → a split whose parents
  are not both in the set, a collapse of a vertex that is the weight-1 centre, a
  flip that changes no vertex. Each is a counted case, and the tests assert the
  counts (`entries carried`, `entries inserted by a split`, `entries retired by
  a collapse`) rather than a reach alone, so a maintenance that silently does
  nothing cannot pass by landing near the right number.
- **[Cognitive complexity]** → `apply_to_mesh`'s loop is already long. The
  capture decision goes in `mesh_stamp_settings`' neighbourhood and the
  maintenance in the adaptive sculptor, so neither consumer grows a branch; the
  implementing PR states the measured scores.

## Migration Plan

Behavioural, not structural. No call signature changes, no descriptor grows, no
format moves. Hosts see a Grab that follows the cursor; there is no opt-out and
none is proposed, because a policy flag here would mean shipping both a brush
that works and a brush that does not and asking the host to pick.

## Open Questions

- Should the fixed path's Grab gain the same capture on `apply_to_multires`'s
  coarse levels, where the level under the brush can change mid-stroke? The
  level sculptor is re-read every stamp today. Measured: nothing in the tree
  drives it. A case is added; if it argues for a different rule, that is a
  follow-up and not this change.
- Does the captured set want an upper bound? On the fixed path it is whatever
  the first gather found (45 here); on the adaptive path D2 lets it grow with
  the splits. Unbounded growth over a very long gesture has not been measured.
- Should a carried entry be PROTECTED from collapse for the length of the
  gesture? That is the other shape of D2 and it was not measured. It would make
  the region's survival structural rather than statistical, at the cost of a
  remesher that refuses work inside a moving ball — which is a smaller version of
  what D3 was rejected for. It becomes the leading alternative if §2.0 shows the
  maintenance cannot hold a high weight.
- Does D2 leave the adaptive surface as refined as P does? P, which is D2's
  centre-follows half without the maintenance, leaves a longest edge of 0.3571
  after the 1.5 drag. D2 adds vertices to the carried region, which is a
  different surface, so 0.3571 is an indication and not a prediction.
