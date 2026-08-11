## Context

The corrugation is born in the COMPOSITION, not in the volume. Measured on the
issue's own repro (unit ball, the cap region, no verb):

| | hard replace | volume alone |
|---|---|---|
| zero-set deviation (bisection) | 0.0 | h²·κ/8, shrinking quadratically |
| worst normal tilt, cell 0.04 | 32.3° | 0.51° |
| worst normal tilt, cell 0.02 | 20.4° | — |

`op_replace(a, b) = min(max(a, −b), b)` keeps `a` in play everywhere `b` says
air. A bake of the same document ties with `a` at every lattice plane, the
branch taken switches at the cell wavelength, and the tetrahedron gradient
pays |b − a| / epsilon in tilt. The issue's resolution-independent 0.00052 is
the raycast's stopping tolerance — both of the reporter's measurement paths
march, so both saw it.

## Goals / Non-Goals

**Goals:** placement of a baked volume whose normals converge with the cell;
no hard edge at the sampled box; a document-sourced relax; byte-identical
behaviour for every existing caller.

**Non-Goals:** changing `op_replace` (every existing document keeps its
meaning); in-place region verbs on the document (the third wish in the issue —
a different, larger change); mirroring a feathered replace (see below).

## Decisions

**The feather rides on the volume, set in `clay_volume_params`.** It is
decided where the volume is baked — the bake knows what the box edge will meet
— and it must survive the document round trip with the samples, so it lives in
`FieldVolume` and its blob header rather than on the op. The header is
self-describing (its size IS the index offset), so the field appends exactly
as the sample Lipschitz did: an old blob reads feather 0, an old reader skips
the new float.

**The crossfade is `a + w·clamp(b − a, ±band)`,** with `w` a smoothstep of the
Chebyshev inset into the sampled box over the feather. Three properties hang
off the band clamp, and they are why it is a clamp rather than a raw lerp:

- *Lipschitz is closed-form.* The blend adds at most `band · 1.5 / feather` —
  the largest value the weight's gradient can multiply — the same shape
  `cfi_relief` and `cfi_transition` already pay. A raw lerp's cost is a bound
  on |b − a| over the whole box, which is a region diagonal: with a
  one-band feather that is a ~75x safe-step collapse against ~1.5 here.
- *Per-brick culling stays exact.* A dropped item can shift `a` only where `a`
  already exceeded the caller's dilation, and a correction no larger than the
  band cannot pull such a value back inside the brick's clamp — provided the
  compiler widens its cull test by that band, which it now does
  (`feather_cull_pad`). A correction that grew with depth would break the
  CullRegion contract unfixably.
- *The degradation is the contract the volume already has.* A verb that moved
  the surface further than the band is expressed only up to the band across
  the margin; the fix is a bigger band at bake time, which is the same rule
  the volume's own accuracy states.

**A mode the compiler emits, not an op a caller picks.** The node's op stays
`Replace`; `ccombine_replace_feather` is emitted only when the placed volume
carries a feather, and reads the box, band and feather from the same blob
header the prim reads, so the two cannot disagree. Feather zero emits
`ccombine_replace` — byte-identical tapes, held by a test that pins the
composed field to `min(max(a, −b), b)` of the separately evaluated operands.

**A truly empty chain degrades to the hard replace; a cull-emptied one does
not.** Blending into the far-field seed would blend a lone volume away to
nothing, so with nothing beneath it the compiler emits the hard mode — but a
chain the CULL emptied must keep the feathered mode and blend against the
seed, because the dropped items are real and the per-brick tape must agree
with the full one. `compile_list` tracks which of the two it is in.

**No mirror participation.** The crossfade follows ONE sampled box; a mirror
copy pre-combined into the same operand would sit outside that box and be
blended away entirely — worse than either behaviour a caller could mean. A
feathered volume skips the layer mirror, documented on the field.

**`relax_from` is sample-then-relax, deliberately.** Relax averages
cell-aligned taps, and a fresh bake's taps ARE the document at those lattice
points, so a fused form has nothing to improve on; the overload (mirroring
flatten's pair) buys one entry point and parity held by a test. Unlike
flatten, relax moves the surface by less than a cell per pass, so the in-place
form on a fresh bake was never inaccurate — the header note says exactly that
instead of implying a defect that is not there.

## Risks / Trade-offs

- **A feathered field is steeper and declares it** (max + band·1.5/feather;
  ~0.31 safe-step scale at the one-band default against 0.577 for a bare
  volume). → That is the honest cost of the blend, it is bounded, and the
  test suite holds the scale above a floor.
- **The cull test widens for every brick of a document containing a feathered
  replace,** costing some cull selectivity near the box. → Correctness first;
  the pad is one band, and zero for every document without a feathered
  replace.
- **Deep verbs need a band that covers them.** → Documented on the field, and
  the same contract the volume already carries; the hard replace's behaviour
  for material beyond the band (a degenerate double-sided shell) was no
  better.
