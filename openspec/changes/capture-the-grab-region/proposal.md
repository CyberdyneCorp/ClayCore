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

**THE DENOMINATOR IS THE NET DISPLACEMENT `|p_n − p_0|`, not the path length**,
and on the curved fixture those differ: a quarter arc of length 0.6 has a chord
of 0.5402. This is the right denominator, because `kernel_grab` is handed
`p_n − p_0` and the weight-1 vertex therefore moves by the CHORD; a rule that
carried a region round an arc and put it 0.6 from where it started would be
wrong. It does mean the curved rows below are normalised by 0.5402 and the
straight ones by 0.6000, so the two are not directly comparable — against the
0.6 path length the curved fixture reads A 41.34% / 41.12% and B 90.03%. Any
test written from §1 must assert the DISPLACEMENT and not the drag's length, or
it will demand 111% of a curve.

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
for C. On the fixed path 10 of 11 stamps move a vertex; on the adaptive path
`apply_to_dynamic` returns 11 of 11, because its return counts stamps that
CHANGED the surface and a remesh is a change. Flipping the sign of the drag
(push-in) and mirroring the fixture to the other pole move A by at most
0.46 points and C by 0.00 / 0.49 points.

**What that invariance does and does not prove.** Re-measured across grid (16,
24, 32) and spacing (0.25, 0.1, 0.05) as well, A stays put — 40.67–43.23% on the
0.6 drag and 18.01–18.46% on the 1.5 one — so the shortfall is genuinely the
mechanism's and not the tessellation's. It is NOT independent of the brush,
however: at the same drag of 0.6 A reaches **22.66% at radius 0.15, 41.10% at
0.30 and 60.56% at 0.50**. "41%" is the number for this preset. The claim that
survives every fixture is the DECAY, not the constant:

Note what A does as the drag gets longer: 41% at 0.6, **18% at 1.5** (and
22.66% → 9.22% at radius 0.15). The shortfall is not a constant discount; it
compounds, because each stamp gathers a weaker region than the last.

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

**And 44% is not the floor.** Re-measured over grid, spacing, radius and detail,
unmaintained B goes much further down, and past today:

| fixture | preset | live of captured | top weight | B | A, same fixture |
|---|---|---|---|---|---|
| push-in −Z 0.6 | grid 32, spacing 0.05, r 0.30, detail 8 | 8 of 69 | 0.070 | 29.02% | 40.99% |
| pull-out −Z 0.6 (mirror) | grid 24, spacing 0.1, r 0.15, detail 4 | **0 of 9** | 0.000 | **5.56%** | 22.24% |
| push-in −Z 0.6 | grid 24, spacing 0.1, r 0.15, detail 4 | **0 of 9** | 0.000 | **6.97%** | 22.30% |
| long push-in −Z 1.5 | grid 24, spacing 0.1, r 0.15, detail 4 | **0 of 9** | 0.000 | **2.81%** | 9.34% |

So an unmaintained captured set does not merely become fixture-dependent, it can
be **worse than shipping today's brush** — 2.8% against 9.3% — and at a small
radius against a coarse detail the remesher retires the set ENTIRELY inside one
stroke. This does not change the proposal; it raises the stake on the
maintenance in §"What this proposes" (2), and it is the evidence behind the open
question recorded in `design.md`.

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
B apply 25 of 26 on the fixed path (26 of 26 on the adaptive one, where the
count includes the remesh).

**C's percentage is a property of the spacing, not of the rule**, which is worth
saying because the table above prints it as though it were a rule's number. On
the same 0.6 pull-out C reads 16.41% at spacing 0.25, 65.84% at 0.1 and 94.49%
at 0.05; on the 1.5 one, 6.56%, 26.49% and 60.33%. Denser stamps mean a smaller
delta per stamp, so the centre falls behind more slowly — it still falls behind.
The stable number is the stamps it manages before it loses the mesh: 2 of 5 at
spacing 0.25, 11 of 26 at 0.1, 40 of 51 at 0.05. C is refuted on the agreement
and on losing the mesh; it is not refuted by "66%", and nothing here should be
read as though it were.

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

## What building it found

`tasks.md` §2.0 said to build the maintenance FIRST, on its own, and measure it
before the rest. It is built, behind a probe switch that is inert at its default,
and it is measured on twelve fixtures — the four §1.5b names, where the
unmaintained set dies, plus §1's two baseline pull-outs as controls, each with
its drag's sign flipped so a number is the mechanism's and not the fixture's.
**The inference in the section above was wrong, and the fix is the alternative
`design.md` named and did not measure.**

The arms, all on `apply_to_dynamic`:

| | region | remesh centre | the captured set across the remesh |
|---|---|---|---|
| **A** | re-gathered every stamp | first stamp | — |
| **B** | captured once | first stamp | nothing maintains it |
| **M** | captured once | follows the stamp | `design.md` D2 exactly as written |
| **MP** | captured once | follows the stamp | D2, plus a collapse may not retire a carried vertex |
| **M+** | captured once | follows the stamp | D2, plus a split with ONE carried parent inserts too |
| **M+P** | captured once | follows the stamp | M+ and MP together |

The fixed path is NOT patched. Candidate B there is one stamp carrying the whole
drag, which is the identity §"Independently re-derived" records, so `fixed B` in
the tables below is measured through the public entry points on an unmodified
`MeshSculptor`.

### 8. The maintenance keeps A high-weight entry alive. It does not keep THE one

Reach, as a share of the drag. Every row is the adaptive path except the last
column:

| fixture (r / detail) | A | B | **M** | **MP** | **M+P** | fixed B |
|---|---|---|---|---|---|---|
| mirror pole −Z 0.6 (0.15 / 4) | 22.24 | **5.56** | 76.38 | 100.02 | **100.02** | 100.00 |
| …sign-flipped | 22.40 | 15.53 | 76.40 | 100.03 | **100.03** | 100.00 |
| push-in −Z 0.6 (0.15 / 4) | 22.30 | **6.97** | 79.44 | 100.01 | **100.01** | 100.00 |
| …sign-flipped | 22.27 | 100.00 | 72.98 | 100.01 | **100.01** | 100.00 |
| long push-in −Z 1.5 (0.15 / 4) | 9.34 | **2.81** | 76.20 | 100.01 | **100.01** | 100.00 |
| …sign-flipped | 9.20 | 100.00 | 75.01 | 100.01 | **100.01** | 100.00 |
| g32 s.05 push-in 0.6 (0.30 / 8) | 41.04 | 29.02 | 97.10 | 100.00 | **100.00** | 100.00 |
| …sign-flipped | 40.99 | 100.00 | 96.09 | 100.00 | **100.00** | 100.00 |
| baseline pull-out 0.6 (0.30 / 8) | 41.34 | 100.00 | 95.54 | 100.00 | **100.00** | 100.00 |
| …sign-flipped | 41.56 | 67.26 | 96.63 | 99.99 | **99.99** | 100.00 |
| baseline pull-out 1.5 (0.30 / 8) | 18.06 | 100.00 | 91.66 | 100.00 | **100.00** | 100.00 |
| …sign-flipped | 18.48 | 66.58 | 93.34 | 100.00 | **100.00** | 100.00 |

A and unmaintained B reproduce §1 and §1.5b to the printed digit — 22.24, 22.30,
9.34, 41.04, 41.34, 18.06 for A and 5.56, 6.97, 2.81, 29.02, 100.00, 67.26,
66.58 for B — from a probe written against the patched engine rather than
against the frozen-workset trick the earlier stage used. That agreement is the
evidence that the switch reaches the code; the counters below are the evidence
that the maintenance does.

**Top surviving weight at the last stamp**, which is the quantity §2.0 asked for
and the one reach follows:

| fixture | B | **M** | **MP** | **M+** | **M+P** |
|---|---|---|---|---|---|
| mirror pole −Z 0.6 | **0.000** | 0.781 | 1.000 | 0.727 | **1.000** |
| push-in −Z 0.6 | **0.000** | 0.805 | 1.000 | 0.782 | **1.000** |
| long push-in −Z 1.5 | **0.000** | 0.752 | 1.000 | 0.781 | **1.000** |
| g32 s.05 push-in 0.6 | 0.070 | 1.000 | 1.000 | 1.000 | **1.000** |
| baseline pull-out 0.6 | 1.000 | 0.953 | 1.000 | 1.000 | **1.000** |
| baseline pull-out 1.5 | 1.000 | 0.915 | 1.000 | 0.958 | **1.000** |

Sign-flipped: M 0.781 / 0.764 / 0.764 / 0.948 / 0.958 / 0.919; MP and M+P 1.000
on all six.

So the answer to §2.0's question is **half yes**. The splits DO repopulate the
core: the carried set grows rather than decaying — 9 entries captured become 59
to 64 live at the end on the radius-0.15 fixtures, 45 become 434 to 538 on the
baseline — and the top surviving weight never goes to zero and never goes near
it. It floors at **0.710** over the twelve fixtures against unmaintained B's
**0.000**. But it is not 1.000: the weight-1 centre IS collapsed, in the first
half of the gesture, and because a split's child takes the MEAN of its parents
nothing can ever put it back. The per-stamp trace of the long push-in shows
exactly that shape — a stair, not a slide, and then a flat line:

```
  stamp   carried in   carry   inserted   retired   moved   top weight
      0            0      40         32         1      42        1.000
      4           38      37          5         6      38        1.000
     12           52      57          7         2      16        1.000
     13           57      57          4         4      14        0.891
     18           64      62          4         6      10        0.805
     21           63      64          2         1       2        0.752
     25..50       64      64          0         0       0        0.752
```

D2's maintenance therefore lands at **72.98–97.10%** reach, not at 100%, and the
cross-representation agreement it leaves is **0.017 – 0.375** in world units —
5x to 1000x WORSE than today's A on the same fixtures (2.9e-4 – 3.6e-3), and one
to two orders outside the 1e-3 §2's constraint and `tasks.md` §4.1 demand. On its
own evidence **D2 as written is not sufficient**.

### 9. The protection settles it, and it costs the opposite of what was feared

Refusing a collapse that would retire a carried vertex — `design.md`'s "other
shape of D2", which it named and did not measure — reaches **99.99–100.03% on
all twelve fixtures**, holds the top surviving weight at **1.000 on all twelve**,
and agrees with the unpatched fixed path to **0.0 – 1.6e-4**:

Agreement, as the absolute difference in world units between the fixed path's
travel and the adaptive path's, over all twelve fixtures:

| arm | agreement, `abs(fixed − adaptive)` |
|---|---|
| A, today (fixed A vs adaptive A) | 2.9e-4 – 3.6e-3 |
| B, unmaintained | 0.0 – 1.46 |
| M, D2 as written | 1.7e-2 – 3.7e-1 |
| **MP and M+P** | **0.0 – 1.6e-4** |

(B, M, MP and M+P are all measured against the same unpatched `fixed B`.)

`design.md` rejected this shape in advance — "a remesher that refuses work inside
a moving ball, which is a smaller version of what D3 was rejected for". **That is
refuted, and by the same number D3 was rejected on.** Longest triangle edge left
on the surface (the sphere starts uniform at 0.0917):

| fixture | A | M | MP | M+ | **M+P** | fixed mesh, no remesher |
|---|---|---|---|---|---|---|
| mirror pole −Z 0.6 | 0.1174 | 0.1891 | 0.1940 | 0.1203 | **0.1174** | 0.5462 |
| push-in −Z 0.6 | 0.1174 | 0.1829 | 0.1867 | 0.1364 | **0.1174** | 0.5328 |
| long push-in −Z 1.5 | 0.1174 | 0.6215 | 0.5635 | 0.3622 | **0.2689** | 1.3150 |
| g32 s.05 push-in 0.6 | 0.0882 | 0.0882 | 0.0882 | 0.0882 | **0.0882** | 0.2524 |
| baseline pull-out 0.6 | 0.1174 | 0.1174 | 0.1174 | 0.1174 | **0.1174** | 0.3541 |
| baseline pull-out 1.5 | 0.1174 | 0.3170 | 0.2218 | 0.2058 | **0.1466** | 0.8176 |

Refusing collapses leaves the surface FINER, not coarser, and on four of the six
fixtures it leaves it exactly as fine as today's A — which is the ceiling,
because A barely moves the tip. The mechanism is visible in the counters: the
collapses being refused are the ones eating the gesture's own region, the carried
set therefore stays dense, and the splits do the refining they were going to do
anyway. It is nothing like D3, which refuses EVERY operation.

What it actually costs, per stroke:

| fixture | collapses performed A / M / MP / **M+P** | refusal events (MP) |
|---|---|---|
| mirror pole −Z 0.6 | 49 / 161 / 66 / **18** | 456 |
| push-in −Z 0.6 | 40 / 140 / 53 / **12** | 381 |
| long push-in −Z 1.5 | 48 / 144 / 53 / **12** | 891 |
| g32 s.05 push-in 0.6 | 126 / 327 / 66 / **68** | 2248 |
| baseline pull-out 0.6 | 254 / 428 / 153 / **96** | 1317 |
| baseline pull-out 1.5 | 325 / 562 / 153 / **96** | 3283 |

The refusal EVENTS are larger than the collapses avoided because the remesher's
three passes re-ask about the same edge on every stamp; the count that means
something is the collapses performed, and it falls to roughly a tenth of D2's.

And the stroke gets **cheaper, not dearer** — one gather per gesture instead of
one per stamp. Median of 21 interleaved strokes, first repeat discarded (21 and
not 200 because one adaptive stroke here is 12–130 ms and the claim is a ratio
between arms on one box, not a latency budget):

| fixture | A | M | **M+P** | M+P / A |
|---|---|---|---|---|
| mirror pole −Z 0.6 | 24.32 | 13.29 | **12.20** | 0.50x |
| push-in −Z 0.6 | 21.57 | 14.29 | **14.77** | 0.68x |
| long push-in −Z 1.5 | 44.16 | 16.74 | **23.29** | 0.53x |
| g32 s.05 push-in 0.6 | 90.95 | 86.49 | **79.11** | 0.87x |
| baseline pull-out 0.6 | 58.30 | 43.86 | **38.14** | 0.65x |
| baseline pull-out 1.5 | 129.04 | 69.78 | **68.34** | 0.53x |

Over all twelve fixtures M+P is **0.39x–0.89x of A**. §6's "B is not cheaper on
the adaptive path" was measured on unmaintained B with the remesh left at the
anchor; with the centre following the stamp and the set maintained, it is.

### 10. Two rules D2 does not have, and the counters found both

**(a) A split with ONE carried parent.** D2 says "a split inside the set". The
counters say a large minority of the splits the remesher runs inside a Grab
region have exactly one carried parent — 42 of the 74 on the first stamp of the
mirror-pole fixture, 100 of 342 on the baseline — and D2 as written drops every
one of them. It need not: the uncarried parent never took any part of the drag,
so its CURRENT position is its captured position, and the child is exactly
reconstructible as the midpoint of `captured_in` and `position_out` with weight
`w_in / 2` — which is D2's own arithmetic with the second weight at zero.

Measured, it does NOT move the reach (M+ 71.39–97.93% against M's 72.98–97.10%,
inside the fixture-to-fixture spread and worse on two rows). What it moves is
the SURFACE and the protection's bill: max edge on the long push-in 0.3622
against M's 0.6215 and, with the protection, **0.2689 against 0.5635**; and the
collapses the protection has to refuse fall from 53 to 12. It is worth having for
those two reasons and not for the reach.

**(b) A carried vertex the REMESHER moved.** This one is not an improvement, it
is a correctness rule D2 is missing. A collapse places its survivor at the
midpoint and `relax_region` slides vertices tangentially; both move vertices that
the gesture is carrying. Without telling the carry, the next stamp writes
`captured + w · drag` and silently undoes the remesher's work. The probe counts
it: **1 to 291 carried vertices are moved by the remesher on every stamp of every
fixture** — 42 on the first stamp of the mirror pole, 291 on the first stamp of
the baseline. The rule is one line (`captured += after − before`) and without it
the relaxation inside a Grab does nothing at all.

### 11. What could and could not be compared

- **Compared.** Adaptive A / B / M / MP / M+ / M+P against each other on twelve
  fixtures, and each against the FIXED path's A and B, both measured through the
  public entry points on an unpatched `MeshSculptor`. The analytic expectation —
  the captured set's weight-1 vertex moves by exactly `|p_n − p_0|`, so reach is
  100.00% and `travel` equals the drag — is met by fixed B on all twelve
  (`travel` = 0.6000 / 1.5000 exactly) and by adaptive M+P to 1.6e-4.
- **Not compared.** `apply_to_multires`, the C ABI, pyclay, the Swift surface and
  the device suite: none is touched by this probe and none should be believed
  from it. The fixed path's 1.85x saving is §6's and is not re-measured here,
  because no fixed-path source changed. And the 100.02 / 100.03 rows are a real
  and tiny OVERSHOOT, not noise: rule (b) lets the weight-1 vertex keep the
  tangential slide the relaxation gave it, so it ends 1.6e-4 past the drag.
- **Probe validity, asserted rather than eyeballed.** Every fixture asserts that
  something was captured, that the remesher split (220–1357 splits per stroke),
  that one row was emitted per stamp, that the surface validates, that M both
  inserted AND retired entries, that MP and M+P refused at least one collapse and
  retired none, that B, M and MP are not byte-identical, and that mode 0 still
  agrees with the unpatched fixed path. All twelve print `PRECONDITIONS ok`.
  With the switch unset the full unit suite passes **2839 of 2839 cases and
  17,931,111 of 17,931,111 assertions** — the same totals §"What was tried and
  refuted" records for the previous stage's patch — so the scaffolding is inert
  at its default and every number above is the rule and not the patch.

### 12. What this section changes in the proposal

`design.md` D2 is corrected rather than kept: the maintenance is necessary and is
not sufficient, and the collapse protection it listed as an open question is
promoted to part of the rule. `design.md` D3 stays rejected and is no longer the
fallback, because the thing it was the fallback FOR now works.

## What this proposes

**Candidate B, with the captured set maintained across the remesh AND protected
from it.** (2) below is revised by §8–§11, which built it and measured it.

1. Grab gathers its region ONCE, at the first stamp, and carries the captured
   items, their captured positions and their weights for the whole gesture. Each
   stamp writes `captured_position + weight * (p_k − p_0)`. Both `apply_to_mesh`
   and `apply_to_dynamic`, together, so the representations keep agreeing.
2. On the adaptive path the captured set is MAINTAINED by the remesh rather than
   rebuilt, and the remesh centre follows the stamp:
   - a split inside the set inserts its new vertex with the midpoint of its
     parents' captured positions and the mean of their weights;
   - a split with exactly ONE carried parent inserts too, at the midpoint of the
     carried parent's captured position and the uncarried parent's current one,
     with half the carried parent's weight;
   - **a collapse may not retire a carried vertex for the length of the
     gesture**, and a collapse of an edge with one carried endpoint therefore
     places its survivor as usual;
   - a carried vertex the REMESHER moved — a collapse's survivor, a relaxation's
     tangential slide — takes the same shift in its captured position.

§3 and §4 are the evidence for (2) being mandatory rather than an optimisation:
the same rule reaches 100% and agrees bit-exactly when the remesher cannot
retire the set, and 2.8–100% with a 0.34 disagreement when it can. §8 and §9 are
the evidence that the maintenance alone is not enough and the protection is what
closes it: 72.98–97.10% and a 0.375 disagreement without it, 99.99–100.03% and a
1.6e-4 disagreement with it, at a lower cost and on a finer surface.

**What is measured and what is inferred, stated plainly.** This paragraph was
written before §8–§11 existed; the hole it names has since been measured and it
was real. **The recommendation's reach and agreement are now measured directly,
in §8 and §9, and the answer was NOT the one inferred here.** What follows is
kept because it is the reasoning the measurement was built to test.

Every number in §1–§7 is a measurement of A, B, P, Q or C. The recommendation —
B with the maintenance — is none of those, and its reach and its agreement are
therefore not measured there; they are inferred from Q, which reaches the whole drag and
agrees to 2e-5 with the identical deformation rule and a remesher that cannot
touch the set. The inference is sound only if the maintenance keeps a HIGH-WEIGHT
entry alive, and the rules in (2) do not guarantee one: a split inserts its child
with the MEAN of its parents' weights, so no maintenance operation can ever
create a weight above the surviving maximum, and a collapse that retires the
weight-1 centre is not recoverable. In the fixtures where unmaintained B fails,
the top surviving weight is 0.090 or 0.000 — the whole high-weight core is gone,
not only its centre. Whether the splits repopulate that core faster than the
collapses eat it is the one quantity this measurement could not reach, and
`tasks.md` §2.0 makes it the first thing the implementing work measures.

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
- **Goldens.** See `tasks.md` §5 for the list, measured by running the suite
  under each rule rather than guessed, and re-measured independently: with the
  captured-set rule made the default in all three consumers, the full suite is
  **2837 of 2839 cases and 17,931,103 of 17,931,111 assertions**, the eight
  failures being `test_dynamic_stroke.cpp:224–232` and `:778–779` and nothing
  else.
- **Prose.** Three sites beyond the ones first listed also state today's rule
  and must move with it: `include/clay/brush/stroke.h:383` (the `apply_to_mesh`
  sentence itself), and `bindings/python/pyclay_module.cpp:5671` and `:8682`,
  which spell "'grab' anchors on the first stamp and drags by the motion between
  stamps" into the Python docstrings. pyclay's BEHAVIOUR does not move; its
  documented rule does.

## What was tried and refuted

- **C, the following centre** — refuted twice: it disagrees between the
  representations by 10% of the drag, and it loses the mesh on a long drag (11
  of 26 stamps applied, 26% reach).
- **B exactly as the issue states it** (captured set, remesh unchanged) —
  refuted: 44–100% reach depending on which pole of the sphere is pulled, and a
  0.34 disagreement between the paths. The diagnosis is measured, not assumed:
  7–13 of 45 captured vertices survive the stroke.
- **M, `design.md` D2 exactly as it was written** (captured set, maintained,
  centre following) — refuted by §8, and this is the finding §2.0 existed to
  produce. It does keep a high-weight entry alive (top surviving weight floors at
  0.710 over twelve fixtures against unmaintained B's 0.000) and the carried set
  GROWS rather than decaying, and it still loses a quarter of the drag —
  72.98–97.10% reach — because the weight-1 centre is collapsed and a split's
  MEAN cannot recreate it. It leaves the two representations 1.7e-2 to 3.7e-1
  apart, worse than today's A. Necessary, not sufficient.
- **The collapse protection was rejected in advance on a cost it does not have.**
  `design.md` called it "a smaller version of what D3 was rejected for". Measured,
  the surface it leaves is FINER than the unprotected maintenance (longest edge
  0.2689 against 0.6215 after a 1.5 push-in) and as fine as today's A on four of
  six fixtures, and the stroke runs at 0.39x–0.89x of A. Checked, refuted,
  recorded — and the rejection is reversed.
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

## Independently re-derived

Every number above was re-measured by a second probe written from the headers
rather than from the first probe, linked against a library rebuilt from a clean
`origin/main` tree in the same worktree, macOS arm64, Release, cpu-only.

A, B-as-one-stamp and C need NO source patch at all and were measured without
one: `kernel_grab` writes `direction * weight` onto the GATHERED position with
no accumulation clamp, so with the region held, `Σ w·(p_k − p_{k−1})` is exactly
`w·(p_n − p_0)` and one stamp carrying the whole drag IS candidate B on a fixed
mesh. The multi-stamp B, P and Q needed one: a `probe_freeze_region` flag that
makes `stamp` reuse the workset it holds. A hand-written re-spelling of A was run
beside `apply_to_mesh` in every row as the probe's own control, and matched it to
every printed digit — a loop that did not match would not have been measuring the
rule it named.

What matched, to the digit: A's 41.10 / 41.34 / 17.99 / 18.06 and every
sign-flipped and mirrored row; C's 65.84 / 55.82 / 26.49 / 22.33; the agreements
(A 3.2e-4 – 5.4e-3, C 7.5e-3 – 6.2e-2, P 4.8e-2 – 5.2e-1, unmaintained B up to
3.4e-1); the topology-off control (B and C bit-exact, A 1.6e-3 and 5.4e-3);
unmaintained B's survivors and their top weights (11/0.090/13/0.661/7/1.000);
P's 79.54 / 66.27 / 79.52 / 65.66 / 66.58; Q at 100% on all six; the longest
edges 0.1174, 1.1227, 0.3571, 0.8176 and 0.3541; C applying 11 of 26; the
gathered-entry counts 402 against 495; and the full-suite totals 2839 cases and
17,931,111 assertions at the default and 2837 / 8 failing under the rule.

The cost was re-measured in a third process, 200 interleaved samples on the fixed
path and 50 on the adaptive one with the first repeat of each discarded, medians:
fixed 0.146 / 0.076 / 0.083 ms for A / B / C on the 0.6 drag (**1.92x**) and
0.299 / 0.162 / 0.118 ms on the 1.5 one (**1.84x**); adaptive 58.4 / 60.0 / 30.4
and 128.2 / 119.8 / 30.7 ms. So the fixed-path saving is 1.8–1.9x rather than a
single 1.85x, and B is not cheaper on the adaptive path, as stated.

What did NOT match is recorded above where it belongs: the curved fixture's
denominator (the chord, not the arc), unmaintained B's floor (2.8%, not 44%),
A's dependence on the brush radius, and C's dependence on the spacing.
