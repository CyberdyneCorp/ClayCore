# Proposal: name the frame an alpha stamp is authored in

## Why

Issue #392 reports that a stroke's template alpha does nothing when correctly
placed and smears when misplaced. **The reported symptom is real and the
reported cause is not.**

`stamps_to_nodes` copies the template node by value, so an alpha reaches every
stamp with its samples intact; and the deformer chain is evaluated at the
ITEM-LOCAL point, so a template alpha authored in the template's frame arrives
in each stamp's own frame — turned by that stamp's rotation, scaled by its
radius — for free. That convention is deliberate and is the only thing that
makes an alpha usable through a stroke at all.

What is broken is one step earlier, and it is the engine's own helper.
`brush::stamp_placement` takes a WORLD surface hit and returns a WORLD frame.
`brush::stamp_deformer` feeds it straight into `Deformer::alpha`, which reads
LOCAL. No conversion exists anywhere. On an item at the identity transform the
two frames coincide — which is every test in the suite and every example — so
this has never shown.

Measured on a sphere at position (0.4, -0.2, 0.1), rotated 0.7 rad about y and
scaled 0.35, with a 0.3-amplitude bump stamped at the world hit: the field
moves **0.000013** at the point that was clicked. Through the conversion it
moves **0.296**.

Both reported symptoms follow from the mis-framing rather than from the stroke:
a centre landing outside the radius makes the region weight zero, and one
landing inside it reads a clamped border texel at every point, which is a
constant offset under the falloff — "nothing" and "a smear".

Separately, `clay_item_add_alpha` accepts a zero `direction` and a non-positive
`radius` and returns `CLAY_OK`. The kernel substitutes local +Z for the first
and floors the second, so both append a deformer that quietly does nothing. The
reporter says this cost them a bad measurement for months.

## What changes

- **`brush::placement_in`** — a world placement expressed in an item's frame,
  and **`brush::stamp_deformer_in`**, which does that conversion and divides
  `extent`, `radius` and `amplitude` by the item's scale.
- **The frame is stated** at all three authoring doors — `clay.h`,
  `scene/types.h` and the pyclay docstring — as local, like a bend curve's
  guide and a lattice's box, with why.
- **A zero direction and a non-positive radius are refused** at both doors,
  and the header says why the mesh brush's all-zeroes convention cannot mean
  the same thing here.
- The first stroke test in the suite that uses an alpha at all.

## Non-goals

- Transforming the alpha inside `stamps_to_nodes`, which is how the issue was
  filed. That would double-apply the stamp transform and desynchronise the tape
  from the bound and the Lipschitz estimate, which read the same local values.
- Porting the mesh brush's "all zeroes means the surface normal" fallback. Both
  mesh implementations resolve it by querying the surface; an SDF item has no
  surface at authoring time.

## Impact

`sdf-kernels` gains the frame contract. `c-abi` gains the two refusals, which
are a visible behaviour change: a call that returned `CLAY_OK` and appended an
inert deformer now returns `CLAY_ERROR_INVALID_ARGUMENT` and appends nothing.
