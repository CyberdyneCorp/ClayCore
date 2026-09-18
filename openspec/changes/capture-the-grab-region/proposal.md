## Why

`brush::apply_to_mesh` anchors Grab on the FIRST stamp and re-gathers the region
around that point on every stamp. The surface has already left it, so the falloff
weights shrink as the stroke goes on and the pull falls short of the drag: a 0.6
pull-out over 11 stamps reaches **41%** of what the artist dragged. Issue #620.

`brush::apply_to_dynamic` (#619, change `stroke-an-adaptive-surface`, design D3)
copies that rule deliberately, so the two representations agree. They do — at
41% each. A Grab that moves the surface 0.246 when the cursor moved 0.600 is not
a Move brush; every host that binds one to Grab is shipping a brush that does not
follow the cursor.

This change measures the two alternatives the issue names, on both paths and on
six fixtures, and proposes one.

## What was measured

Probe: a scratch binary linked against this tree's `libclaycore.a`, driving
`brush::apply_to_mesh` and `brush::apply_to_dynamic` directly, with a probe-only
patch in `src/brush/stroke.cpp`, `src/mesh/sculpt.cpp` and
`src/mesh/dynamic_sculpt.cpp` selecting the anchor rule from an environment
variable. The patch is measurement only and is not part of this change.

Fixture: `cube_sphere(24, 1.0)` (six 24x24 quad grids projected onto the unit
sphere, 3750 vertices), default `DynamicTopologySettings` (enabled,
brush-relative, detail 8), preset radius 0.3, spacing 0.1 — which is the 11-stamp
0.6 pull-out the issue reports. Release, cpu-only preset, macOS arm64.

**Reach** is defined so it survives a sign flip and a curve: the fixture is the
UNIT sphere, so a vertex the drag moved is one that has left it (outward for a
pull, inward for a push). Among those and only those, the furthest travel along
the drag from the first stamp, as a share of the drag. Taking the surface's
extent along the axis — the issue's phrasing — agrees with this on a pull-out and
measures nothing at all on a push-in, whose dent never touches the ball's extent.

The candidates:

| | centre | region | displacement written |
|---|---|---|---|
| **A** (today) | first stamp | re-gathered every stamp | `gathered_position + w * (p_k - p_{k-1})` |
| **B** | first stamp | gathered ONCE at the first stamp | `captured_position + w * (p_k - p_0)` |
| **P** | cursor | captured (B), remesh centre follows | as B |
| **Q** | first stamp | captured (B), no remesh inside the gesture; one remesh at the tip on the last stamp | as B |
| **C** | cursor | re-gathered every stamp | `gathered_position + w * (p_k - p_{k-1})` |

P and Q are not in the issue. They were added because B on the adaptive surface
did not behave, and they are what identified why.

### 1. The issue reproduces, and 41% is the mechanism's, not the fixture's

| fixture | path | A | B | P | Q | C |
|---|---|---|---|---|---|---|
| pull-out +Z 0.6 | fixed | **41.10%** | 100.00% | 100.00% | 100.00% | **65.84%** |
| | adaptive | **41.34%** | 100.00% | 79.54% | 100.00% | **55.82%** |
| push-in -Z 0.6 (sign flipped) | fixed | 41.16% | 100.00% | 100.00% | 100.00% | 65.84% |
| | adaptive | 41.56% | 67.26% | 66.27% | 100.00% | 64.59% |
| pull-out -Z 0.6 (mirrored fixture) | fixed | 41.10% | 100.00% | 100.00% | 100.00% | 65.84% |
| | adaptive | 41.48% | 43.95% | 79.52% | 100.00% | 56.31% |
| curved 0.6 (quarter arc +Z -> +X) | fixed | 45.92% | 100.00% | 100.00% | 100.00% | 74.01% |
| | adaptive | 45.68% | 100.00% | 91.12% | 100.00% | 76.60% |
| long pull-out +Z 1.5 | fixed | 17.99% | 100.00% | 100.00% | 100.00% | 26.49% |
| | adaptive | 18.06% | 100.00% | 65.66% | 100.00% | 22.33% |
| long push-in -Z 1.5 (sign flipped) | fixed | 18.46% | 100.00% | 100.00% | 100.00% | 26.49% |
| | adaptive | 18.48% | 66.58% | 66.58% | 100.00% | 25.93% |

The issue's table is reproduced exactly: **41% / 41%** for A and **66% / 56%**
for C, 10 of 11 stamps moving a vertex on both paths. Flipping the sign of the
drag (push-in) and mirroring the fixture to the other pole move A by at most
0.46 points and C by 0.00 / 0.49 points, so those numbers are the mechanism's.

Note what A does as the drag gets longer: 41% at 0.6, **18% at 1.5**. The
shortfall is not a constant discount; it compounds, because each stamp gathers a
weaker region than the last.

### 2. Cross-representation agreement — the constraint #619 holds

Reported as |fixed − adaptive| in world units on a drag of 0.6 (or 1.5):

| fixture | A | B | P | Q | C |
|---|---|---|---|---|---|
| pull-out +Z 0.6 | 1.4e-3 | **0.0** | 1.2e-1 | 1.7e-5 | 6.0e-2 |
| push-in -Z 0.6 | 2.4e-3 | 2.0e-1 | 2.0e-1 | 1.9e-5 | 7.5e-3 |
| pull-out -Z 0.6 (mirrored) | 2.2e-3 | **3.4e-1** | 1.2e-1 | 1.7e-5 | 5.7e-2 |
| curved 0.6 | 1.3e-3 | 0.0 | 4.8e-2 | 2.0e-5 | 1.4e-2 |
| long pull-out +Z 1.5 | 1.0e-3 | 0.0 | **5.2e-1** | 1.0e-6 | 6.2e-2 |
| long push-in -Z 1.5 | 3.2e-4 | 5.0e-1 | 5.0e-1 | 1.0e-6 | 8.4e-3 |

Two things this refutes:

- **C breaks the agreement outright**, by 10% of the drag — 40x worse than
  today. It is not a candidate.
- **B, as the issue states it, also breaks it** — by up to 0.34 on a 0.6 drag
  (57% of it) — and does so *fixture-dependently*: 100% on one pole, 44% on the
  other. A rule whose answer depends on which pole of a sphere you pull is not a
  rule.

### 3. Why B is fixture-dependent: the remesher eats the captured set

The captured set is 45 vertices. Counted at the end of the stroke, how many are
still live, and the largest falloff weight among the survivors:

| fixture | captured | still live after 11 stamps | top surviving weight | reach |
|---|---|---|---|---|
| pull-out +Z 0.6 | 45 | 11 | 1.000 | 100% |
| push-in -Z 0.6 | 45 | 13 | 0.661 | 67% |
| pull-out -Z 0.6 (mirrored) | 45 | **7** | **0.090** | **44%** |
| curved 0.6 | 45 | 11 | 1.000 | 100% |

The adaptive surface remeshes after every Grab stamp and a collapse retires
vertex ids. **76–84% of the captured set is retired inside one 11-stamp stroke**,
and the reach follows whether the weight-1 centre happened to survive. That is
the whole of B's fixture dependence, and it is not a tuning problem.

The control settles it. With `DynamicTopologySettings::enabled = false`:

| fixture | path | A | B | C |
|---|---|---|---|---|
| pull-out +Z 0.6, topology OFF | fixed | 41.10% | 100.00% | 65.84% |
| | adaptive | 41.37% | 100.00% | 65.84% |
| agreement | | 1.6e-3 | **0.000000** | **0.000000** |
| long pull-out +Z 1.5, topology OFF | fixed | 17.99% | 100.00% | 26.49% |
| | adaptive | 18.36% | 100.00% | 26.49% |
| agreement | | 5.4e-3 | **0.000000** | **0.000000** |

With the remesher out of the picture B is **bit-exact** across the two
representations — better than today's A, which disagrees by 1.6e-3 to 5.4e-3
because the two gathers are not the same gather. So the deformation rule B names
does not endanger #619's promise; it tightens it. What endangers it is
exclusively a captured set nothing maintains.

### 4. Q: the same rule with the remesher held off, as the second control

Q keeps the captured set intact by not remeshing inside the gesture and running
one remesh at the tip on the last stamp. It reaches **100% on both paths on all
six fixtures**, agrees to **1.0e-6 – 2.0e-5** (50–300x tighter than today), and
leaves a surface that validates (`validate_dynamic_surface().ok`, captured_live
43–45 of 45).

Q is not the proposal, because of what it costs — see §6 — but it is the second
half of the proof: with the identical deformation rule, the only variable
between B (44–100%, disagreeing by up to 0.34) and Q (100%, agreeing to 2e-5) is
whether the remesher is allowed to retire the captured set mid-gesture.

### 5. C is refuted a second time: it loses the mesh on a long drag

On the 1.5 pull-out, C applies **11 of 26 stamps** on both paths and then stops
moving anything — the centre vertex moves by its falloff weight rather than the
whole delta, the brush falls behind the cursor, and a few stamps later it is
outside its own radius. This is the failure `src/brush/stroke.cpp` already
documents for Snakehook-anchored-on-the-cursor, reproduced here for Grab. A and
B apply 25 of 26.

### 6. Cost per stroke

Median of 200 timed strokes (fixed) and 50 (adaptive), Release, first run of
each arm discarded, two independent processes; the two processes' medians agree
to within 11%:

| fixture | path | A | B | P | Q | C |
|---|---|---|---|---|---|---|
| pull-out 0.6 | fixed, ms | 0.145 / 0.154 | **0.077 / 0.083** | 0.077 / 0.079 | 0.074 / 0.078 | 0.086 / 0.085 |
| | adaptive, ms | 60.3 / 67.1 | 61.9 / 69.5 | 32.1 / 31.8 | **3.90 / 3.95** | 31.7 / 31.1 |
| long 1.5 | fixed, ms | 0.311 / 0.303 | **0.169 / 0.160** | 0.161 / 0.179 | 0.166 / 0.160 | 0.120 / 0.117 |
| | adaptive, ms | 128.3 / 126.4 | 124.1 / 118.0 | 33.8 / 35.7 | **2.74 / 2.68** | 31.2 / 30.4 |

- **B is 1.85x CHEAPER than today on the fixed path** (0.145 → 0.077 ms; 0.311 →
  0.169 ms): it gathers once instead of once per stamp. The gathered-vertex
  counter says the same thing about where the saving is — B considers 495
  entries (45 x 11, the captured set re-read) against A's 402 *gathered* ones,
  and does 1 walk instead of 11.
- **B is not cheaper on the adaptive path** (61.9 vs 60.3 ms): there the remesh
  dominates, and B remeshes as much as A does.
- **A following centre does NOT cost 101x here.** The SDF move-coalescing path
  measured that (`move-coalescing-is-bit-exact`); on both mesh paths C is
  *cheaper* than A (0.086 vs 0.145 ms fixed; 31.7 vs 60.3 ms adaptive) — and it
  is cheaper for the bad reason, because its region shrinks as it loses the
  surface. Checked, refuted, recorded.

### 7. What the adaptive AFTER remesh does, and whether it has to move

Grab's remesh timing is `AfterBrush` and it runs at `brush.center`, which under
A and B is the first stamp — the header says so under "what it does not do".

Longest triangle edge left on the surface (the sphere starts uniform at 0.0917):

| fixture | A | B | P | Q | fixed mesh (no remesher) |
|---|---|---|---|---|---|
| pull-out 0.6 | 0.1174 | 0.2227 | 0.1174 | 0.2658 | 0.3541 |
| long pull-out 1.5 | 0.1174 | 1.1227 | 0.3571 | 0.8176 | 0.8176 |

- Under **A** the tip barely moves, so a remesh at the anchor is a remesh at the
  deformation, and the surface stays refined (0.1174).
- Under **B with the remesh left at the anchor**, the tip is 1.5 away and the
  remesh never reaches it: max edge 1.1227, *worse than doing nothing*.
- Moving it with the cursor (**P**) recovers the refinement (0.3571) and costs
  the reach, because following the cursor is what retires the captured vertices
  fastest.
- **Q** does not remesh during the drag at all, so it ends exactly as coarse as
  a fixed mesh (0.8176). It reaches 100% and it stops being an adaptive surface
  for the duration of the gesture.

So the answer to the issue's "should the AFTER remesh move with the anchor" is
**yes, it must** — a remesh left at the first stamp while the surface is dragged
1.5 away refines nothing — **and moving it is not sufficient**, because a
remesher that runs over the captured set retires it.

## What this proposes

**Candidate B, with the captured set maintained across the remesh.**

1. Grab gathers its region ONCE, at the first stamp, and carries the captured
   items, their captured positions and their weights for the whole gesture. Each
   stamp writes `captured_position + weight * (p_k − p_0)`. Both `apply_to_mesh`
   and `apply_to_dynamic`, together, so the representations keep agreeing.
2. On the adaptive path the captured set is MAINTAINED by the remesh rather than
   rebuilt: a split inside the set inserts its new vertex with the midpoint of
   its parents' captured positions and the mean of their weights; a collapse
   removes the vertex it retires. The remesh centre follows the stamp.

§3 and §4 are the evidence for (2) being mandatory rather than an optimisation:
the same rule reaches 100% and agrees bit-exactly when the remesher cannot
retire the set, and 44–100% with a 0.34 disagreement when it can.

## What it costs and what it breaks

- **The fixed and multiresolution paths get 1.85x cheaper and need no new
  machinery.** One gather per gesture instead of one per stamp.
- **The adaptive path needs the maintenance work**, which is new: the remesh must
  publish its splits and collapses to the workset that the gesture is carrying.
  That is the substance of this change and the risk in it. Its cost is
  O(splits + collapses) per stamp — already-paid traffic — and it is bounded by
  counters, so the tests gate a count and not a clock.
- **A Grab now moves the surface as far as the cursor moved.** On a 1.5 drag that
  is 5.6x further than today. Any host-side feel tuned against the 41% will feel
  different; this is the point of the change and the release notes must say so.
- **The SDF Move brush does not change**, and `openspec/specs/brush-engine`
  already records that it "moves LESS than the displacement asked for, because
  the region weight is taken at the sample point rather than at its preimage".
  After this change the mesh Grab and the SDF move disagree about reach. That is
  a real divergence between two brushes an artist thinks of as one, and the
  change must state it rather than leave it to be discovered.
- **Goldens.** See `tasks.md` §1 for the list, measured by running the suite
  under each rule rather than guessed.

## What was tried and refuted

- **C, the following centre** — refuted twice: it disagrees between the
  representations by 10% of the drag, and it loses the mesh on a long drag (11
  of 26 stamps applied, 26% reach).
- **B exactly as the issue states it** (captured set, remesh unchanged) —
  refuted: 44–100% reach depending on which pole of the sphere is pulled, and a
  0.34 disagreement between the paths. The diagnosis is measured, not assumed:
  7–13 of 45 captured vertices survive the stroke.
- **P, moving the remesh centre to the cursor** — refuted as a fix on its own: it
  restores the refinement (max edge 0.3571 vs 1.1227) and still disagrees by up
  to 0.52.
- **Q, suppressing the remesh inside the gesture** — a legitimate fallback and
  the cheapest thing measured (2.7 ms against 128 ms, 47x), rejected as the
  proposal because it makes an adaptive surface behave like a fixed one for the
  length of a Grab: max edge 0.8176, identical to the fixed mesh.
- **The scaffolding is inert at its default.** With the probe patch applied and
  the switch unset, the full unit suite passes 2839 of 2839 cases and 17,931,111
  of 17,931,111 assertions, so every difference in the tables above is the rule
  and not the patch. Under the captured-set rule the same suite fails 2 cases
  and 8 assertions, and they are the two named in `tasks.md` §5.
- **A probe bug worth recording.** The first version of the mode switch cached
  the environment variable in a function-local static, so all four rules
  measured as bit-identical — the classic "the probe never reached the code".
  The second version of Q re-gathered on the last stamp and applied the
  accumulated drag twice, reading 180% reach. Both were caught by the counters
  (`vertices_considered` identical across modes; reach above 100%) and not by
  the eye.
