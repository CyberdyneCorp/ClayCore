## Why

**A pressure-driven radius costs 101x**, and so does a centre that follows the
finger. Both are the natural gesture; both defeat the engine's coalescing
completely.

A live drag replaces the leading run of grabs it already emitted rather than
stacking, so a long drag costs one warp. `continues_gesture` decides "already
emitted by me" by comparing type, centre and radius with **raw float `==`, no
epsilon**. That is true only of a drag holding centre and radius fixed.

Measured, 60-frame drag, 2-item layer, 15,625 probes:

| mode | warps | eval | vs baseline |
|---|---:|---:|---:|
| anchored | 2 | 0.121 ms | 2.1x |
| fixed centre through a float round trip (same **bits**) | 2 | 0.121 ms | 2.1x |
| recomputed anchor (algebraically equal, bitwise not) | 58 | 2.697 ms | 47.6x |
| **pressure-driven radius** | **120** | **5.726 ms** | **101x** |
| finger-following centre | 120 | 5.740 ms | 101x |

Linear in frame count: 30/60/120/240 frames give 60/120/240/480 warps when
following, and 2 at every count when anchored.

**The radius is in the key just as hard as the centre.** A pressure-modulated
radius — the obvious ZBrush-parity feature — was not merely unsupported, it was
101x. Blender disabling pressure for Grab is not an oversight.

## Not an epsilon, and the reason is correctness rather than taste

Matching centres within a tolerance would paper over the recomputed-anchor case
while silently folding two genuinely **distinct** gestures — a re-grab a hair
from the last — into one. That changes the document rather than speeding it up.
Quantising to a grid is worse: it makes the fold depend on where the gesture
happens to sit relative to the grid.

The original design was right that a drag with a different centre is a
different gesture. The defect is that **the library had no way to know which it
was**, and inferred it from float identity.

## The fix

The host names the gesture. One `uint64` set at touch-down and held for the
drag; the engine stamps it on every grab it emits and matches on it when it is
non-zero.

Zero means "not said" and the bit-equality rule applies exactly as before —
which is what a caller compiled against the older struct gets, and what a
loaded document gets.

## Deliberately not serialised

The id names a gesture **in flight** and means nothing once it ends; the chain
a file carries is the finished result. A loaded document reads zero and falls
back to the old rule, which is what it should do — and writing it would change
the scene format for a value no reader could use.

Deformer serialisation is field-by-field, so the new member costs the format
nothing.

## What building it found

**The first attempt put the field on the wrong struct.** The anchor was
`std::uint8_t ease = 0;`, which matches `Transition` before it matches
`Deformer` — both carry an easing. Caught by checking which struct the line had
landed in rather than that the edit had applied; the anchor is now the `ext[]`
comment, which is unique to `Deformer`.

## What changes for a caller

Additive behind `struct_size`. A caller that sets nothing keeps today's
behaviour exactly, asserted by a test that drives the same drag through the new
struct with a zero id and through a **truncated** struct predating the field,
and compares both the warp count and the field.
