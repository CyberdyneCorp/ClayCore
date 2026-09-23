## Why a threshold rather than a flag

A boolean "hard gate" still has to choose a cutting value, and the only honest
ones are 0 (any mask at all refuses) and 0.5 (an arbitrary midpoint). A float
lets the host say which, and carries its own off switch at zero.

## Why the threshold also stops the scaling

The obvious conservative reading — refuse at or above the threshold, keep
scaling below it — does not solve the reported problem. A solid dab at mask 0.3
would still be written with weight 0.7 and dithered, so the speckled crust
survives in the band the threshold does not cover. That is the thing the
reporting host says it cannot explain to a sculptor.

So a non-zero threshold makes the mask BINARY over the whole range: refuse at or
above, unscaled below. That is what "binary occupancy" means, and it is one
switch rather than two interacting ones.

The cost is that the field changes the meaning of the mask rather than only
adding a cutoff. The documentation says so in those words, because a caller
reading "threshold" and expecting a floor under the existing behaviour would be
surprised by the skirt becoming solid.

## Why the default is exact rather than merely close

Zero is not "a threshold above every mask value". It is a separate branch that
leaves the existing expression untouched, so a caller that does not set the
field gets the same writes, the same dither sequence and the same seed
behaviour. A default implemented as `threshold = nextafter(1, 2)` would change
the code path for every existing caller to prove a point about uniformity.
