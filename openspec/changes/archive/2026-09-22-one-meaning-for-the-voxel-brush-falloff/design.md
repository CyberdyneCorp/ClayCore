## Why not map the names onto nearest eases

Because it cannot be done faithfully in two of the four cases.
`cregion_weight` is `cease(type, clamp(1 - d, 0, 1))`, so it always carries the
(1 - d) factor. No ease reproduces `Constant`, which is flat 1 across the ball,
and none reproduces `Gaussian`'s exp(-4.5 d^2). A mapping would have to
approximate exactly the two curves a host cannot otherwise reach, and would
leave the field still meaning something different here than elsewhere.

Reading the voxel table instead makes `falloff` mean one thing, which is the
constraint the reporting host asked for and the only one that removes the class
rather than an instance.

## What this changes for a caller

`Constant` becomes a rigid pull inside the ball rather than a linear taper.
Measured on a slab at brush size 8, displacement 0.25 world units, rise in cells
at the centre and at half the radius:

    falloff     before        after
    Constant    2  /  1       4  /  3
    Linear      2  /  1       2  /  1
    Smooth      2  /  1       2  /  1
    Gaussian    1  /  1       2  /  1

Linear and Smooth read the same at this resolution — their curves differ but
nearest-cell resampling quantises the difference away on this fixture. That is
the representation, not the fix.

## Why a discontinuity at the rim is acceptable here

`Constant` now steps from full displacement to none at the rim. On an SDF that
would wreck the Lipschitz bound sphere tracing depends on; on a voxel grid
occupancy is already binary and nearest-cell, and every other voxel verb's
`Constant` already behaves this way. The grab was the odd one out.

## What moves under a host, in the words of the one who found it

Reported back by ClaySpaceDesktop after the fix merged, and recorded here
because the release notes are written from these changes rather than from the
commit log.

A host that passes `Constant` to a **grab** in order to get full coverage will
lose its taper on 0.117.0. Theirs does: their fix for a speckled-crust defect
writes grid dabs solid by passing `Constant`, and their voxel drag inherited it.
Under the cast that meant `ease_linear`, so the drag tapered — which is also the
accident that let them believe their own change had made the drag rigid when it
had not.

The remedy is the correspondence this change names: use `Linear` for the grab.
Both are (1 - d), so it is the same curve the cast was accidentally delivering,
asked for by its right name. Coverage and pull profile are separate questions
for a drag — every cell still resamples, and the taper is what makes it read as
Move rather than as a translation.

So the release-note line is: **a `Constant` voxel grab now pulls rigidly**, and a
host that wanted the old taper should ask for `Linear`.
