# Proposal: sweeping profiles along a guide

## Why

`add-loft-opcode` interpolates profiles along the item's Z axis. This carries
the same profiles along a **guide curve** — 3DCoat's Swept Along Guide and
Swept N, and what they use to loft whole base meshes from a boundary curve and
a profile.

## What building loft changed about this row

The plan called this "the same opcode with a count", on the reading that loft
would ship with exactly two profiles. Loft shipped with N, so that framing is
gone and the real content is the word the roadmap always used: **guide**.

Three things follow, none of which the count framing predicted:

**The frame has to be transported, not computed.** A Frenet frame flips at an
inflection point and is undefined on a straight segment — a sweep built on one
would twist visibly where the guide happens to straighten. The fix is
parallel transport, which is sequential along the curve and therefore cannot be
computed per sample. So the compiler walks the guide once, transports a frame
along it, and writes a frame per vertex; the kernel interpolates between the
two bracketing it. That is the shape of the change.

**Evaluation needs a closest-point search.** Mapping a query point into the
guide's frame means finding the nearest point on the guide first. That is a
loop over segments per sample — the same cost `ctape_stroke` already pays, and
the same pattern, so it is not new ground.

**The Lipschitz story is worse than loft's, and unbounded in one case.** A
sweep compresses space on the inside of a bend: a point at perpendicular
offset `r` inside a bend of radius `R` is squeezed by `R / (R - r)`, which
diverges as `r` approaches `R`. `cfi_wrap_around` already has exactly this
shape — thickness against wrap radius — so the engine has the pattern. What
the engine cannot do is refuse: a guide is editable after the fact, so a
profile that outgrows the guide's tightest bend has to *degrade*, not fail.
It reports a large Lipschitz and an inexact field, which makes the raymarcher
crawl rather than lie. The alternative — silently reporting Lipschitz 1 — puts
holes in the surface.

## What Changes

- **A `Swept` primitive and a `ctape_swept` opcode**, carrying a tessellated
  guide with a transported frame per vertex, plus the loft's profile list.
- **The guide is a control-point curve**, reusing `add-curve-objects` whole:
  the same point types, the same tolerance, the same tessellator. A guide is
  not a new kind of curve.
- **Profiles are distributed along the guide's arc length**, so a bunched
  guide does not bunch the profiles.
- **Exactness and a curvature-driven Lipschitz**, declared from the guide's
  tightest turn against the widest profile.

## What this change does not do

- **No closed guides.** Parallel transport around a loop does not generally
  return to its starting frame — the leftover twist is real geometry
  (holonomy), not a bug, and closing the seam needs either a compensating twist
  distributed along the guide or an accepted discontinuity. Both are decisions
  worth making deliberately rather than as a footnote here.
- **No per-profile stations and no twist along the guide.** Both are one float
  per station and purely additive.
- **No self-intersection repair.** A profile wider than the guide's tightest
  bend radius folds the sweep through itself. The field degrades honestly —
  inexact, small steps — rather than being silently wrong, and the example
  shows the boundary.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `scene-model`, `python-bindings`, `c-abi`.

## Impact

- `include/clay/kernel/lift.h`, `include/clay/kernel/tape.h`,
  `include/clay/kernel/exactness.h`, `include/clay/scene/types.h`,
  `src/scene/tape_build.cpp`, `src/scene/bounds.cpp`, `src/scene/commands.cpp`,
  both bindings, the parity corpus, tests, docs, an example.
- ABI 0.19.0 — additive.
