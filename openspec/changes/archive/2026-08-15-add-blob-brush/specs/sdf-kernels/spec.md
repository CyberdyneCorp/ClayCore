# sdf-kernels — noise that stays under the brush

Delta for `add-blob-brush`.

## ADDED Requirements

### Requirement: A blob deformer applies noise with finite support
The kernel dialect SHALL provide a blob deformer: fractal noise offsetting the distance, weighted by a radial falloff, so an artist gets ZBrush's irregular swelling under the brush where `draw` gives a smooth one.

It SHALL reuse the EXISTING fractal and the EXISTING region weight rather than defining either again. Two noises would be two things to keep in step, and a second definition of reach is the bug avoided elsewhere by keeping one definition of the local cull test.

It SHALL offset the DISTANCE rather than warping the point, as the whole-item noise does and for the same reason: the wanted irregularity is the surface moving along its own normal, where a point warp would slide material sideways.

Outside the radius the field SHALL be untouched exactly, not nearly — that is what makes it a brush rather than a modifier, and what lets the influence bound stay tight.

The amplitude SHALL be signed and SHALL NOT be split into two verbs. The noise is signed, so one dab both swells and eats in, which is what reads as blobby rather than as a uniform bulge.

The declared Lipschitz factor SHALL charge BOTH the noise's own gradient and the REGION's: the offset is amplitude times weight times fractal, so by the product rule the weight varying over a constant noise contributes too. A tight radius has a steep weight even where the noise is flat, and that is exactly where a blob is used — so a bound charging only the noise would under-bound the common case.

The influence bound SHALL grow by the AMPLITUDE. The whole-item noise stores its amplitude in a different slot, so sharing its hull case would dilate by a coordinate — a wrong bound that shows up as culled-away geometry rather than as a failure.

The seed SHALL be an ordinary parameter rather than global state, so two items with the same seed look the same and an item's appearance never depends on the order it was compiled in.

It SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene in which BOTH the falloff and the fractal are non-trivial, since a backend that applied one and ignored the other would otherwise pass.

#### Scenario: Past the radius the field is untouched
- **WHEN** a blob is applied and the field is evaluated outside its radius
- **THEN** the value equals the undeformed field's, to within float equality rather than a tolerance

#### Scenario: One dab swells and eats in
- **WHEN** the field is compared to the undeformed one at many points inside the radius
- **THEN** some points moved outward and some inward

#### Scenario: A tighter radius costs more step scale
- **WHEN** the same blob is declared with a small radius and a large one
- **THEN** the smaller reports the tighter safe step scale, because the region's own gradient is charged

#### Scenario: The influence bound follows the amplitude
- **WHEN** a blob whose region centre is far from the origin is bounded
- **THEN** the item's bound grows by the amplitude and not by the centre's coordinate
