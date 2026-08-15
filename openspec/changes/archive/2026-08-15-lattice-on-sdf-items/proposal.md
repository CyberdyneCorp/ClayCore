# A lattice on SDF items

## Why

`lattice-on-a-mesh` shipped the cage where ZBrush and Blender actually put it —
on vertices, forward, with no compromise. This is the other half, and it is the
one #116 called the interesting design problem.

An SDF item has no vertices. Evaluating an implicit field means asking *what
material is at this point*, so every claycore deformer is an **inverse point
map**, and forward FFD has no closed-form inverse. #116 records three answers:

- **(a) Newton-invert per sample** — exact, and puts iteration inside every GPU
  kernel. That breaks the single-source dialect the five backends share.
- **(b) Bake through a `FieldVolume`** — exact forward FFD, sampled once, and
  the cage stops being editable afterwards.
- **(c) Author the cage as the INVERSE warp** — closed-form, portable, and not
  quite the exact inverse of the forward map.

## What Changes

**(c).** The cage's control-point offsets are what the artist DRAGGED, and a
point samples the undeformed field at `p - Bernstein(offsets, param(p))` — the
minus is the inverse map. So dragging a control point moves material *with* it,
which is what a lattice means and what the mesh lattice does with a plus for the
same reason. Storing the already-negated sample offset would make the two
lattices disagree about what a positive offset means.

That approximation is the house style rather than a shortcut — the basis is
evaluated at the sample point rather than at its preimage. But the *character*
is not `grab`'s, and saying it were would be wrong: the inverse cage is not the
exact inverse of forward FFD, and the two differ by a term proportional to how
the basis VARIES along the displacement. So the error points the way the basis
gradient does — it over-travels a drag toward rising weight and under-travels
one pointing away. `grab`'s weight always falls off along its drag, which is why
that one always under-travels; a lattice does not inherit the sign.

**Measured**, on a sphere with one control point dragged 0.4 against the same
cage applied forward on a mesh layer: the difference is **under 1.5% of the
drag**. Small, signed, and stated rather than characterised by analogy.

Everything else follows the mesh lattice deliberately, so the two cannot drift:
offsets rather than positions (an untouched cage is exactly the identity), the
same trivariate Bernstein (2 per axis is exactly trilinear, corners are
interpolated), and the same clamp outside the box (material there travels
rigidly rather than being drawn onto the cage).

### Where it differs from the mesh lattice, and why

**Divisions are capped at 4 per axis, not 32.** A mesh lattice evaluates once
per vertex; this one evaluates **per sample**, inside the raymarcher's inner
loop, at a cost of `nx * ny * nz` multiply-adds each time. A 32³ cage would be
32,768 of them per sample. The cap is stated where a caller meets it rather than
discovered as a frame-rate cliff.

**The cage lives in the item's LOCAL space**, like every other deformer. A
layer-level cage resolved into per-item warps — the `brush::move_brush` pattern
#116 points at — is a separate, additive change and is not assumed here.

## Impact

- `sdf-kernels` — a new deformer, its field-info bound, its influence hull
- `scene-model` — the cage rides the deformer's blob payload, the machinery
  `bend-along-a-curve` added
- `bindings` — `pyclay` and the C ABI, plus a parity-corpus scene

## Out of scope

- **The layer-level resolver.** One world cage over a whole layer, resolved into
  per-item lattices. It needs an answer for items under rotation (a
  world-axis-aligned cage is not axis-aligned in a rotated item's local space),
  which wants a transform in the payload. Worth doing; not assumed here.
- **N-divisible cages beyond 4.** The per-sample cost is the reason, and it is
  the same reason ZBrush's own lattice is not driven at arbitrary resolution.
