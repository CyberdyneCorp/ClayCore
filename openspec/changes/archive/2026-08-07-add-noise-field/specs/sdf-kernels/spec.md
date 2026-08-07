# sdf-kernels — a noise field

Delta for `add-noise-field`.

## ADDED Requirements

### Requirement: Noise is reproducible on every backend
The noise SHALL be hashed from INTEGER lattice coordinates using integer operations, so that every backend produces the same values within the parity tolerance.

A float hash SHALL NOT be used. Cross-backend parity is tolerance-based rather than bit-exact — 1e-6 relative on the CPU backends and 1e-4 on the GPU ones — and the usual `fract(sin(...) * large)` construction amplifies a units-in-the-last-place disagreement in `sin` into an O(1) disagreement in its output, because taking a fractional part of a large product is chaotic by design. It would fail parity on the first case.

#### Scenario: Every backend agrees
- **WHEN** a document containing noise is evaluated on each registered backend
- **THEN** the results agree within the same tolerance every other primitive is held to

#### Scenario: The same seed gives the same field
- **WHEN** two items are given the same noise parameters and seed
- **THEN** their fields are identical

#### Scenario: A different seed gives a different field
- **WHEN** only the seed is changed
- **THEN** the field differs

### Requirement: Noise displaces the distance, as an ordinary deformer
Noise SHALL contribute a distance OFFSET, in the same place `displace` does, rather than warping the point. It SHALL be an ordinary deformer so that it serializes, crosses the C ABI and runs on every backend without a mechanism of its own.

It SHALL be fractal, summing octaves, because one octave of gradient noise is smooth blobs and a weathered surface wants detail at several scales.

#### Scenario: Noise roughens a surface
- **WHEN** a smooth shape is given a noise deformer with a non-zero amplitude
- **THEN** its surface is irregular rather than smooth, and the deviation is bounded by the amplitude

#### Scenario: Zero amplitude changes nothing
- **WHEN** the amplitude is zero
- **THEN** the field is unchanged everywhere

#### Scenario: More octaves add finer detail
- **WHEN** the octave count is raised with the amplitude held
- **THEN** the surface gains detail at smaller scales without the overall deviation growing without bound

#### Scenario: It is irregular, not periodic
- **WHEN** the field is sampled along a line at the noise's own frequency
- **THEN** it does not repeat, which is what distinguishes it from the sine the displace deformer uses

### Requirement: The steepness noise adds is declared
Offsetting the distance by a function raises the field's slope by that function's gradient, so the tape's Lipschitz SHALL carry it — as it already does for `displace`. The bound SHALL account for every octave, because each octave has a higher frequency than the last and so a steeper gradient.

#### Scenario: The field stops being exact
- **WHEN** a document containing a noise deformer is compiled
- **THEN** it reports the field as inexact and the safe step scale is below one

#### Scenario: More amplitude or frequency declares more
- **WHEN** either the amplitude or the frequency is raised
- **THEN** the reported Lipschitz rises and the safe step scale falls

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a noisy shape
- **THEN** it stops at the surface rather than passing through it
