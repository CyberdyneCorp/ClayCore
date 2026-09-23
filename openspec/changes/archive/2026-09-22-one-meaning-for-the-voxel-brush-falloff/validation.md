## The curves

Sampled at d = 0, 0.25, 0.5, 0.75 — what the name means against what the cast
delivered:

    falloff     the name's curve            what the cast delivered
    Constant    1.000 1.000 1.000 1.000     1.000 0.750 0.500 0.250
    Linear      1.000 0.750 0.500 0.250     1.000 0.844 0.500 0.156
    Smooth      1.000 0.844 0.500 0.156     1.000 0.896 0.500 0.104
    Gaussian    1.000 0.755 0.325 0.080     1.000 0.562 0.250 0.062

Each falloff delivered the NEXT one's curve, and Gaussian delivered (1 - d)^2,
which has no name in either table.

## What a caller sees

Slab, brush size 8, displacement 0.25 world units, rise in cells at the centre
and at half the radius:

    falloff     before        after
    Constant    2  /  1       4  /  3
    Linear      2  /  1       2  /  1
    Smooth      2  /  1       2  /  1
    Gaussian    1  /  1       2  /  1

Constant is the visible change: a rigid pull rather than a linear taper, which
is the curve a host asking for a solid drag wants and could not previously
reach. Linear and Smooth read the same at this resolution — their curves differ
but nearest-cell resampling quantises the difference away on this fixture.

## There were TWO copies of the map

`GrabTransaction::update` carries the same resample as `sculpt_grab`, with its
own comment saying why they must not drift:

> If this resample ever drifted from that one, a host would get a different
> result from a drag than from the single call it is meant to be equivalent to.

Fixing only `sculpt_grab` broke `grab gesture: one update equals the stateless
call` — the comment's promise working. Both are fixed.

## Three existing expectations encoded the old curve

None was wrong about the engine; each was fitted to the cast.

- **the bodily-translation case** asserted the trailing face vacates ONE cell.
  Its own prose says "the span moved bodily"; the numbers under it described a
  shear, because Constant was delivering Linear's taper. Now literally bodily,
  and it vacates two.
- **the SDF-agreement case** paired the voxel DEFAULT (Constant) against
  `Deformer::grab(..., ease=0)` = ease_linear. That agreed only because Constant
  was cast into ease 0 — the agreement WAS the defect. It now pairs
  `BrushFalloff::Linear` with ease 0, which is a true correspondence: both are
  (1 - d).
- **the front-gate margin case** reasons about "0.5625 of the ungated pull once
  the falloff is folded in", arithmetic that only holds for a falloff that
  tapers. It now names Linear instead of taking the default. Under a rigid
  Constant the dead zone is genuinely narrower, which is a consequence of the
  curve rather than a gate that stopped working.

## Mutation

With the cast restored, exactly ONE test case fails — the new one. Full suite
with the fix: 2,785 cases, 17,876,900 assertions. c-abi, layering,
kernel-dialect and task-symbols all OK; clean under `-Wshadow -Wall -Wextra
-Werror`.
