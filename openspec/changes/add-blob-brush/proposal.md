# Blob: noise that stays under the brush

## Why

The last named-feature hole in #118's workstream A, and the roadmap has held it
open waiting for one thing: *"unblocked by `add-noise-field`"*.

ZBrush's Blob gives an irregular swelling under the brush where `draw` gives a
smooth one. Both `docs/07` and `docs/sculpt_comparison.md` list it as absent.

The fractal it needs already exists — `cnoise_fbm`, shipped with the `noise`
deformer — and so does the falloff — `cregion_weight`, shared by `grab`, `pose`
and `magnify`. What did not exist is the one line that puts them together.

## What Changes

`Deformer::blob(centre, radius, amplitude, frequency, octaves, gain, seed, ease)`:
the same fractal the whole-item `noise` offsets the distance by, multiplied by
the same radial falloff the region deformers use.

**A distance offset, not a point warp**, for the reason `noise` is one: the
irregularity wanted here is the surface moving in and out along its own normal.
Warping the point would slide material sideways instead.

**One signed amplitude, not two verbs.** The noise is signed, so a single dab
both swells and eats in — which is what reads as blobby rather than as a
uniform bulge. A negative amplitude inverts the whole response; it is not a
second brush, in the way `magnify`'s negative strength is not `pinch`'s
sibling but its identity.

## What it is not

**Not a new noise.** The fractal, its octaves, its gain and its integer-hash
seed are `add-noise-field`'s, unchanged. Two noises in the tree would be two
things to keep in step, and a host that matched a blob to a whole-item noise
would find they disagreed.

**Not a new falloff.** `cregion_weight` is what decides reach for every region
deformer; a second definition is the bug this codebase avoided once by keeping
one definition of the local cull test.

## Impact

`sdf-kernels` gains the deformer and its bound. `bindings` gains it in both
surfaces, and the parity corpus gains a scene — deliberately one where the
falloff and the fractal are both non-trivial, since a backend can get either
wrong independently.
