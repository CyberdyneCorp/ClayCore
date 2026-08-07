# Proposal: a noise field

## Why

`add-magnify-pinch` carved Blob out and recorded it as blocked on this: Blob's
character is an *irregular* surface response, and the engine has no irregularity
to offer. The nearest thing is the `displace` deformer, whose sine is regular by
construction — it gives an even corrugation, which is precisely what Blob is not.

Noise is also what several other things want: weathering and erosion on a hard
surface, organic break-up on an otherwise machined form, and a displacement that
does not read as a wave.

## The design question is which noise, and it is settled by a number

The roadmap listed three open questions — which noise, whether it lives in the
kernel dialect so all four backends agree, and how a seed reaches the tape. The
first two answer each other.

Cross-backend parity is **tolerance-based, not bit-exact**: 1e-6 relative on the
CPU backends and 1e-4 on the GPU ones. That tolerance is comfortable for
ordinary arithmetic, and it is fatal for the usual float hash. The classic
`fract(sin(dot(p, k)) * 43758.5453)` takes whatever `sin` does differently
between libm, CUDA, Metal and OpenCL — a few units in the last place — multiplies
it by forty-three thousand, and then takes a fractional part, which is chaotic
by design. A 1e-7 disagreement in `sin` becomes an O(1) disagreement in the
output. It would fail parity on the first case.

So the hash must be **integer**: the same integer operations produce the same
bits on every backend, which is what makes a lattice noise reproducible at all.
That decides the noise too — gradient (Perlin-style) noise on an integer
lattice, with an integer hash choosing the gradients. Not simplex, whose
advantage is speed in higher dimensions we do not have, and not a float hash of
any kind.

The cost is that the shim gains an unsigned integer type. That is worth naming
because the shim is, by its own comment, "the ONLY header allowed to know which
backend is compiling" — no other kernel header uses an integer today, so this is
a genuine widening of the dialect rather than a new function inside it.

## Where it goes

Beside `displace`, which is already exactly this shape: a deformer contributing
a distance OFFSET rather than warping the point. `d' = d - amplitude * fbm(p)`.
That gets noise the whole deformer chain — finite params, serialization,
the C ABI, all four backends — without inventing a mechanism.

Fractal by octaves, because one octave of gradient noise is smooth blobs and
what a weathered surface wants is detail at several scales.

## The seed

An ordinary deformer parameter, hashed together with the lattice coordinate.
Not a global, and not state on the document: two items with the same seed
SHOULD look the same, and an item's appearance must not depend on the order it
was compiled in.

## What Changes

- **`cnoise_fbm`** in the kernel dialect: integer-hashed gradient noise with
  octaves, and the `cuint` type it needs.
- **`cdeform_noise`**: amplitude, frequency, octaves, gain, seed, contributing a
  distance offset like `displace`.
- **`cfi_noise`**: offsetting the distance by a function of bounded gradient
  raises the Lipschitz by that gradient, as `cfi_displace` already does — but
  summed over the octaves, because each one is steeper than the last.
- **The C ABI, Python bindings**, tests including a cross-backend parity case,
  and an example.

## What this change does not do

- **No Blob brush.** This is the field Blob needs; the brush that uses it is a
  separate row, now unblocked.
- **No curl or domain-warped noise.** Both are compositions of this one and are
  additive.
- **No worley/cellular.** Different character, different cost; a second noise
  type is a row of its own if something wants it.
- **No noise on colour.** Displacement only.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `c-abi`, `python-bindings`.

## Impact

- `include/clay/kernel/shim.h` gains an integer type; a new
  `include/clay/kernel/noise.h`; `tape.h`, `exactness.h`, `scene/types.h`,
  `src/scene/bounds.cpp`, the C ABI, the Python bindings, the parity corpus,
  tests, docs, an example.
